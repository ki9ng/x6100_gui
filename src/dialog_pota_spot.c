/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot dialog
 *
 *  Screens:
 *    1. Park # input  → [SPOT] [CANCEL]
 *    2. Progress       → status label + FT8 slot counter + [CANCEL]
 *    3. Result         → success or failure message + [OK]
 *
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#include "dialog_pota_spot.h"

#include "buttons.h"
#include "cfg/cfg.h"
#include "dialog.h"
#include "events.h"
#include "lvgl/lvgl.h"
#include "msg.h"
#include "params/params.h"
#include "pota_spot.h"
#include "styles.h"
#include "textarea_window.h"

#include <stdio.h>
#include <string.h>

/* ─── layout constants ───────────────────────────────────────────────────── */

#define DIALOG_W    400
#define DIALOG_H    300

/* ─── widgets ────────────────────────────────────────────────────────────── */

typedef enum {
    SCREEN_INPUT = 0,
    SCREEN_PROGRESS,
    SCREEN_RESULT,
} dialog_screen_t;

static dialog_screen_t cur_screen;

/* containers — only one visible at a time */
static lv_obj_t *cont_input;
static lv_obj_t *cont_progress;
static lv_obj_t *cont_result;

/* input screen widgets */
static lv_obj_t *label_park;      /* "Park Reference:" */
static lv_obj_t *label_park_val;  /* current value being typed */

/* progress screen widgets */
static lv_obj_t *label_status;    /* "Posting via WiFi…" / "Transmitting (2/4)" */
static lv_obj_t *bar_progress;    /* FT8 slot progress bar (hidden on WiFi path) */

/* result screen widgets */
static lv_obj_t *label_result;    /* "✓ Spot posted!" / "✗ Spot failed" */
static lv_obj_t *label_result_detail;

/* typed park reference accumulator */
static char park_buf[16];

/* ─── forward decls ──────────────────────────────────────────────────────── */

static void show_screen(dialog_screen_t s);
static void spot_status_cb(pota_spot_state_t state, int tx_num);
static void open_keyboard(void);
static bool keyboard_ok_cb(void);
static bool keyboard_cancel_cb(void);

/* ─── button callbacks ───────────────────────────────────────────────────── */

static void btn_spot_cb(struct button_item_t *btn) {
    (void)btn;
    if (strlen(park_buf) == 0) {
        msg_schedule_text_fmt("Enter a park reference first");
        return;
    }
    show_screen(SCREEN_PROGRESS);
    pota_spot_start(park_buf, spot_status_cb);
}

static void btn_cancel_cb(struct button_item_t *btn) {
    (void)btn;
    if (pota_spot_busy()) {
        pota_spot_cancel();
    } else {
        dialog_destruct(dialog_pota_spot);
    }
}

static void btn_ok_cb(struct button_item_t *btn) {
    (void)btn;
    dialog_destruct(dialog_pota_spot);
}

static void btn_enter_park_cb(struct button_item_t *btn) {
    (void)btn;
    open_keyboard();
}

/* ─── button pages ───────────────────────────────────────────────────────── */

static button_item_t btn_enter = {
    .type  = BTN_TEXT,
    .label = "Enter\nPark #",
    .press = btn_enter_park_cb,
};
static button_item_t btn_spot = {
    .type  = BTN_TEXT,
    .label = "SPOT",
    .press = btn_spot_cb,
};
static button_item_t btn_cancel = {
    .type  = BTN_TEXT,
    .label = "CANCEL",
    .press = btn_cancel_cb,
};
static button_item_t btn_ok = {
    .type  = BTN_TEXT,
    .label = "OK",
    .press = btn_ok_cb,
};

static buttons_page_t page_input    = {{ &btn_enter, &btn_spot,  &btn_cancel }};
static buttons_page_t page_progress = {{ &btn_cancel }};
static buttons_page_t page_result   = {{ &btn_ok }};

/* ─── screen management ───────────────────────────────────────────────────── */

static void show_screen(dialog_screen_t s) {
    cur_screen = s;

    lv_obj_add_flag(cont_input,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cont_progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cont_result,   LV_OBJ_FLAG_HIDDEN);

    switch (s) {
        case SCREEN_INPUT:
            lv_obj_clear_flag(cont_input, LV_OBJ_FLAG_HIDDEN);
            buttons_load_page(&page_input);
            break;
        case SCREEN_PROGRESS:
            lv_obj_clear_flag(cont_progress, LV_OBJ_FLAG_HIDDEN);
            buttons_load_page(&page_progress);
            break;
        case SCREEN_RESULT:
            lv_obj_clear_flag(cont_result, LV_OBJ_FLAG_HIDDEN);
            buttons_load_page(&page_result);
            break;
    }
}

/* ─── spot status callback (called on LVGL thread via scheduler) ─────────── */

static void spot_status_cb(pota_spot_state_t state, int tx_num) {
    char buf[64];

    switch (state) {
        case POTA_SPOT_WIFI_POSTING:
            lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(label_status, "Posting via WiFi...");
            break;

        case POTA_SPOT_FT8_WAITING:
            lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(label_status, "No WiFi — waiting for FT8 slot...");
            break;

        case POTA_SPOT_FT8_TRANSMITTING:
            lv_obj_clear_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
            snprintf(buf, sizeof(buf), "Transmitting FT8 (%d/4)", tx_num);
            lv_label_set_text(label_status, buf);
            lv_bar_set_value(bar_progress, tx_num * 25, LV_ANIM_ON);
            break;

        case POTA_SPOT_DONE:
            snprintf(buf, sizeof(buf),
                     LV_SYMBOL_OK " Spot posted!\n%s", park_buf);
            lv_label_set_text(label_result, buf);
            lv_label_set_text(label_result_detail,
                              "Check pota.app to confirm.");
            show_screen(SCREEN_RESULT);
            break;

        case POTA_SPOT_FAILED_NO_WIFI:
            lv_label_set_text(label_result,
                              LV_SYMBOL_CLOSE " Spot failed");
            lv_label_set_text(label_result_detail,
                              "No WiFi. No SOTAmat config found.\n"
                              "Connect WiFi or load sotamat.blob\n"
                              "to /mnt/DATA/ on the SD card.");
            show_screen(SCREEN_RESULT);
            break;

        case POTA_SPOT_FAILED_API:
            lv_label_set_text(label_result,
                              LV_SYMBOL_CLOSE " API error");
            lv_label_set_text(label_result_detail,
                              "WiFi connected but POTA API\n"
                              "returned an error. Try again.");
            show_screen(SCREEN_RESULT);
            break;

        case POTA_SPOT_FAILED_FT8:
            lv_label_set_text(label_result,
                              LV_SYMBOL_CLOSE " FT8 TX failed");
            lv_label_set_text(label_result_detail,
                              "FT8 transmission error.");
            show_screen(SCREEN_RESULT);
            break;

        case POTA_SPOT_CANCELLED:
            show_screen(SCREEN_INPUT);
            break;

        default:
            break;
    }
}

/* ─── keyboard for park reference entry ──────────────────────────────────── */

static void open_keyboard(void) {
    textarea_window_open(keyboard_ok_cb, keyboard_cancel_cb);
    lv_obj_t *ta = textarea_window_text();

    /* Accept digits, letters, and hyphen */
    lv_textarea_set_accepted_chars(ta,
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "-");
    lv_textarea_set_max_length(ta, 10);
    lv_textarea_set_placeholder_text(ta, "K-1234");

    /* Pre-fill with last used park or param default */
    const char *prefill = pota_spot_last_park();
    if (prefill && strlen(prefill) > 0) {
        textarea_window_set(prefill);
    }
}

static bool keyboard_ok_cb(void) {
    const char *val = textarea_window_get();
    if (!val || strlen(val) == 0) {
        msg_schedule_text_fmt("Park reference required");
        return false;
    }
    strncpy(park_buf, val, sizeof(park_buf) - 1);
    park_buf[sizeof(park_buf) - 1] = '\0';

    /* Convert to uppercase */
    for (char *c = park_buf; *c; c++) {
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    }

    /* Update display label */
    lv_label_set_text(label_park_val, park_buf);
    textarea_window_close();
    return true;
}

static bool keyboard_cancel_cb(void) {
    textarea_window_close();
    return true;
}

/* ─── dialog lifecycle ───────────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    cur_screen = SCREEN_INPUT;
    park_buf[0] = '\0';

    /* ── Input container ── */
    cont_input = lv_obj_create(dialog.obj);
    lv_obj_set_size(cont_input, DIALOG_W, DIALOG_H);
    lv_obj_set_pos(cont_input, 0, 0);
    lv_obj_set_style_bg_opa(cont_input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont_input, 0, 0);
    lv_obj_set_style_pad_all(cont_input, 16, 0);
    lv_obj_clear_flag(cont_input, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont_input, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont_input,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Title */
    lv_obj_t *title = lv_label_create(cont_input);
    lv_label_set_text(title, "POTA Self-Spot");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    /* Park label */
    label_park = lv_label_create(cont_input);
    lv_label_set_text(label_park, "Park Reference:");
    lv_obj_set_style_text_color(label_park, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_pad_top(label_park, 20, 0);

    /* Park value */
    label_park_val = lv_label_create(cont_input);
    lv_label_set_text(label_park_val, "— tap Enter Park # to set —");
    lv_obj_set_style_text_color(label_park_val, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_park_val, &lv_font_montserrat_18, 0);

    /* Current op frequency (informational) */
    lv_obj_t *freq_label = lv_label_create(cont_input);
    char freq_buf[32];
    int32_t f = subject_get_int(cfg_cur.fg_freq);
    snprintf(freq_buf, sizeof(freq_buf), "Freq: %.3f MHz  %s",
             f / 1e6, mode_str());
    lv_label_set_text(freq_label, freq_buf);
    lv_obj_set_style_text_color(freq_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_pad_top(freq_label, 8, 0);

    /* ── Progress container ── */
    cont_progress = lv_obj_create(dialog.obj);
    lv_obj_set_size(cont_progress, DIALOG_W, DIALOG_H);
    lv_obj_set_pos(cont_progress, 0, 0);
    lv_obj_set_style_bg_opa(cont_progress, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont_progress, 0, 0);
    lv_obj_set_style_pad_all(cont_progress, 24, 0);
    lv_obj_clear_flag(cont_progress, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont_progress, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont_progress,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    label_status = lv_label_create(cont_progress);
    lv_label_set_text(label_status, "Starting...");
    lv_obj_set_style_text_color(label_status, lv_color_white(), 0);

    bar_progress = lv_bar_create(cont_progress);
    lv_obj_set_size(bar_progress, 260, 16);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_pad_top(bar_progress, 16, 0);
    lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);

    /* ── Result container ── */
    cont_result = lv_obj_create(dialog.obj);
    lv_obj_set_size(cont_result, DIALOG_W, DIALOG_H);
    lv_obj_set_pos(cont_result, 0, 0);
    lv_obj_set_style_bg_opa(cont_result, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont_result, 0, 0);
    lv_obj_set_style_pad_all(cont_result, 24, 0);
    lv_obj_clear_flag(cont_result, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont_result, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont_result,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    label_result = lv_label_create(cont_result);
    lv_label_set_text(label_result, "");
    lv_obj_set_style_text_font(label_result, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label_result, lv_color_white(), 0);

    label_result_detail = lv_label_create(cont_result);
    lv_label_set_text(label_result_detail, "");
    lv_label_set_long_mode(label_result_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label_result_detail, DIALOG_W - 48);
    lv_obj_set_style_text_color(label_result_detail, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_pad_top(label_result_detail, 12, 0);
    lv_obj_set_style_text_align(label_result_detail, LV_TEXT_ALIGN_CENTER, 0);

    /* Show first screen */
    show_screen(SCREEN_INPUT);
}

static void destruct_cb(void) {
    if (pota_spot_busy()) {
        pota_spot_cancel();
    }
    textarea_window_close();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    switch (key) {
        case LV_KEY_ESC:
            if (cur_screen == SCREEN_PROGRESS && pota_spot_busy()) {
                pota_spot_cancel();
            } else {
                dialog_destruct(dialog_pota_spot);
            }
            break;
        default:
            break;
    }
}

/* ─── dialog descriptor ──────────────────────────────────────────────────── */

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .rotary_cb    = NULL,
    .key_cb       = key_cb,
};

dialog_t *dialog_pota_spot = &dialog;
