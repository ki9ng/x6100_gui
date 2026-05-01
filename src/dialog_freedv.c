/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI — FreeDV full-screen dialog (ki9ng fork)
 *
 *  Full-screen overlay opened from APP → FreeDV button.
 *  Shows mode label, sync status LED, SNR, callsign display, and a
 *  scrollable event log.  Audio is owned by freedv.c; this file is UI only.
 *
 *  Layout (dialog container is 796×348, per dialog_style in styles.c):
 *    y=  0..50   Top bar: mode label | sync LED | SNR label
 *    y= 50..53   Horizontal divider
 *    y= 56..132  Callsign display area
 *    y=136..346  Scrollable log table
 */

#include "dialog_freedv.h"

#include "dialog.h"
#include "freedv.h"
#include "buttons.h"
#include "keyboard.h"
#include "main_screen.h"
#include "styles.h"
#include "radio.h"
#include "events.h"
#include "lvgl/lvgl.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ── Log table row types ────────────────────────────────────────────────── */

typedef enum {
    LOG_INFO     = 0,
    LOG_CALLSIGN = 1,
    LOG_TX       = 2,
    LOG_ERROR    = 3,
} log_type_t;

typedef struct {
    log_type_t type;
} cell_tag_t;

#define LOG_MAX_ROWS   128
#define LOG_CLEAN_ROWS  32

/* ── Widget pointers (valid while dialog is open) ───────────────────────── */

static lv_obj_t   *mode_label;
static lv_obj_t   *sync_led;
static lv_obj_t   *sync_text;
static lv_obj_t   *snr_label;
static lv_obj_t   *callsign_label;
static lv_obj_t   *log_table;
static lv_timer_t *stats_timer;

static int         last_sync = -1;

/* ── Forward declarations ───────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void stats_timer_cb(lv_timer_t *t);
static void log_draw_begin_cb(lv_event_t *e);
static void fdv_log(log_type_t type, const char *fmt, ...);

/* ── Dialog descriptor ──────────────────────────────────────────────────── */

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .rotary_cb    = NULL,
    .btn_page     = &buttons_page_freedv,
    .key_cb       = key_cb,
};

dialog_t *dialog_freedv = &dialog;

/* ── Log table ──────────────────────────────────────────────────────────── */

static void fdv_log(log_type_t type, const char *fmt, ...) {
    if (!log_table) return;

    char text[80];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    uint16_t rows = lv_table_get_row_cnt(log_table);

    /* Trim oldest rows when table gets too long */
    if (rows >= LOG_MAX_ROWS) {
        lv_table_t *t = (lv_table_t *)log_table;
        lv_coord_t removed_h = 0;
        for (uint16_t i = 0; i < LOG_CLEAN_ROWS; i++)
            removed_h += t->row_h[i];
        if (t->row_act > LOG_CLEAN_ROWS)
            t->row_act -= LOG_CLEAN_ROWS;
        else
            t->row_act = 0;
        for (uint16_t i = LOG_CLEAN_ROWS; i < rows; i++) {
            lv_table_set_cell_value(log_table, i - LOG_CLEAN_ROWS, 0,
                lv_table_get_cell_value(log_table, i, 0));
            cell_tag_t *old = lv_table_get_cell_user_data(log_table, i, 0);
            cell_tag_t *cpy = malloc(sizeof(cell_tag_t));
            if (cpy && old) *cpy = *old;
            lv_table_set_cell_user_data(log_table, i - LOG_CLEAN_ROWS, 0, cpy);
        }
        rows -= LOG_CLEAN_ROWS;
        lv_table_set_row_cnt(log_table, rows);
        lv_obj_scroll_by_bounded(log_table, 0, removed_h, LV_ANIM_OFF);
    }

    cell_tag_t *tag = malloc(sizeof(cell_tag_t));
    if (tag) tag->type = type;

    lv_table_set_cell_value(log_table, rows, 0, text);
    lv_table_set_cell_user_data(log_table, rows, 0, tag);

    /* Auto-scroll to bottom */
    uint16_t cur_row, cur_col;
    lv_table_get_selected_cell(log_table, &cur_row, &cur_col);
    if (cur_row + 1 >= rows) {
        int32_t key = LV_KEY_DOWN;
        lv_event_send(log_table, LV_EVENT_KEY, &key);
    }
}

/* Color each log row by type */
static void log_draw_begin_cb(lv_event_t *e) {
    lv_obj_t               *obj = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS) return;

    uint32_t    row  = dsc->id / lv_table_get_col_cnt(obj);
    uint32_t    col  = dsc->id - row * lv_table_get_col_cnt(obj);
    cell_tag_t *tag  = lv_table_get_cell_user_data(obj, row, col);

    dsc->rect_dsc->bg_opa = LV_OPA_50;

    if (!tag) {
        dsc->rect_dsc->bg_color = lv_color_hex(0x303030);
        return;
    }

    switch (tag->type) {
        case LOG_INFO:
            dsc->rect_dsc->bg_color = lv_color_hex(0x303030);
            break;
        case LOG_CALLSIGN:
            dsc->rect_dsc->bg_color = lv_color_hex(0x00DD00);
            break;
        case LOG_TX:
            dsc->rect_dsc->bg_color = lv_color_hex(0x0000FF);
            break;
        case LOG_ERROR:
            dsc->rect_dsc->bg_color = lv_color_hex(0xDD0000);
            break;
    }
}

/* ── Stats polling timer (200 ms, LVGL thread) ──────────────────────────── */

static void stats_timer_cb(lv_timer_t *t) {
    int   sync = 0;
    float snr  = 0.0f;
    freedv_get_stats(&sync, &snr);

    /* Sync LED */
    lv_obj_set_style_bg_color(sync_led,
        sync ? lv_color_hex(0x00CC00) : lv_color_hex(0xCC0000), 0);
    lv_label_set_text(sync_text, sync ? "SYNC" : "NOSY");

    /* SNR */
    char buf[32];
    if (sync)
        snprintf(buf, sizeof(buf), "SNR: %+.0f dB", snr);
    else
        snprintf(buf, sizeof(buf), "SNR: ---");
    lv_label_set_text(snr_label, buf);

    /* Mode label (reflects any change via encoder knob) */
    char mbuf[24];
    snprintf(mbuf, sizeof(mbuf), "Mode: %s", freedv_mode_label(fdv_get_mode()));
    lv_label_set_text(mode_label, mbuf);

    /* Log sync transitions (skip first poll to avoid spurious "Sync lost") */
    if (last_sync >= 0 && sync != last_sync) {
        fdv_log(LOG_INFO, sync ? "Sync acquired" : "Sync lost");
    }
    last_sync = sync;
}

/* ── Key handler ────────────────────────────────────────────────────────── */

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct(&dialog);
            break;

        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;

        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
    }
}

/* ── construct_cb ───────────────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    lv_obj_set_style_pad_all(dialog.obj, 0, 0);

    /* ── Top bar (y=0..50) ───────────────────────────────────────── */

    mode_label = lv_label_create(dialog.obj);
    lv_obj_set_pos(mode_label, 5, 8);
    lv_obj_set_size(mode_label, 155, 36);
    lv_label_set_text_fmt(mode_label, "Mode: %s", freedv_mode_label(fdv_get_mode()));
    lv_obj_set_style_text_font(mode_label, &sony_24, 0);

    sync_led = lv_obj_create(dialog.obj);
    lv_obj_set_size(sync_led, 22, 22);
    lv_obj_set_pos(sync_led, 168, 14);
    lv_obj_set_style_radius(sync_led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(sync_led, 1, 0);
    lv_obj_set_style_border_color(sync_led, lv_color_hex(0x888888), 0);
    lv_obj_set_style_bg_color(sync_led, lv_color_hex(0xCC0000), 0);
    lv_obj_set_style_bg_opa(sync_led, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sync_led, LV_OBJ_FLAG_SCROLLABLE);

    sync_text = lv_label_create(dialog.obj);
    lv_obj_set_pos(sync_text, 198, 8);
    lv_obj_set_size(sync_text, 80, 36);
    lv_label_set_text(sync_text, "NOSY");
    lv_obj_set_style_text_font(sync_text, &sony_24, 0);
    lv_obj_set_style_text_color(sync_text, lv_color_hex(0xCC0000), 0);

    snr_label = lv_label_create(dialog.obj);
    lv_obj_set_pos(snr_label, 290, 8);
    lv_obj_set_size(snr_label, 220, 36);
    lv_label_set_text(snr_label, "SNR: ---");
    lv_obj_set_style_text_font(snr_label, &sony_24, 0);

    /* Divider */
    lv_obj_t *div = lv_obj_create(dialog.obj);
    lv_obj_set_size(div, 796, 2);
    lv_obj_set_pos(div, 0, 51);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Callsign display (y=56..132) ────────────────────────────── */

    lv_obj_t *cs_hdr = lv_label_create(dialog.obj);
    lv_obj_set_pos(cs_hdr, 0, 56);
    lv_obj_set_size(cs_hdr, 796, 26);
    lv_label_set_text(cs_hdr, "Last received callsign:");
    lv_obj_set_style_text_align(cs_hdr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(cs_hdr, &sony_20, 0);
    lv_obj_set_style_text_color(cs_hdr, lv_color_hex(0x888888), 0);

    callsign_label = lv_label_create(dialog.obj);
    lv_obj_set_pos(callsign_label, 0, 84);
    lv_obj_set_size(callsign_label, 796, 48);
    lv_label_set_text(callsign_label, "---");
    lv_obj_set_style_text_align(callsign_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(callsign_label, &sony_44, 0);
    lv_obj_set_style_text_color(callsign_label, lv_color_hex(0x00DD00), 0);

    /* ── Log table (y=136..346) ───────────────────────────────────── */

    log_table = lv_table_create(dialog.obj);
    lv_obj_set_pos(log_table, 0, 136);
    lv_obj_set_size(log_table, 796, 208);

    lv_table_set_col_cnt(log_table, 1);
    lv_table_set_col_width(log_table, 0, 794);

    lv_obj_remove_style(log_table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_style_bg_opa(log_table, 192, LV_PART_MAIN);
    lv_obj_set_style_bg_color(log_table, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(log_table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(log_table, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_border_opa(log_table, 128, LV_PART_MAIN);

    lv_obj_set_style_border_width(log_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(log_table, lv_color_hex(0xC0C0C0), LV_PART_ITEMS);
    lv_obj_set_style_pad_top(log_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(log_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(log_table, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_font(log_table, &sony_20, LV_PART_ITEMS);

    lv_obj_add_event_cb(log_table, log_draw_begin_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(log_table, key_cb, LV_EVENT_KEY, NULL);

    lv_group_add_obj(keyboard_group, log_table);
    lv_group_set_editing(keyboard_group, true);

    /* ── Init ────────────────────────────────────────────────────── */

    mem_save(MEM_BACKUP_ID);
    main_screen_lock_mode(true);
    main_screen_lock_ab(true);
    main_screen_lock_freq(true);
    main_screen_lock_band(true);

    if (fdv_get_mode() == FDV_MODE_OFF)
        fdv_set_mode(FDV_MODE_700D);

    last_sync   = -1;
    stats_timer = lv_timer_create(stats_timer_cb, 200, NULL);

    fdv_log(LOG_INFO, "FreeDV %s RX started", freedv_mode_label(fdv_get_mode()));
}

/* ── destruct_cb ────────────────────────────────────────────────────────── */

static void destruct_cb(void) {
    if (stats_timer) {
        lv_timer_del(stats_timer);
        stats_timer = NULL;
    }

    mem_load(MEM_BACKUP_ID);
    main_screen_lock_mode(false);
    main_screen_lock_ab(false);
    main_screen_lock_freq(false);
    main_screen_lock_band(false);

    /* Null out widget pointers so fdv_log is a no-op after close */
    sync_led       = NULL;
    sync_text      = NULL;
    snr_label      = NULL;
    mode_label     = NULL;
    callsign_label = NULL;
    log_table      = NULL;
    last_sync      = -1;
}
