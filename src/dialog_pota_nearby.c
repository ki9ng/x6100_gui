/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA Nearby dialog — GPS-sorted park list → MFK knob select → press to confirm.
 *
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#include "dialog_pota_nearby.h"
#include "dialog_pota_spot.h"

#include "buttons.h"
#include "dialog.h"
#include "keyboard.h"
#include "pota_db.h"
#include "pota_parks.h"
#include "gps.h"
#include "msg.h"
#include "styles.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── tunables ──────────────────────────────────────────────────────────── */

#define MAX_ROWS    200
#define LIST_W      776     /* dialog is 796px wide, 10px padding each side */
#define LIST_H      300
#define TITLE_H     32

/* ── forward declarations ──────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void btn_cancel_cb(struct button_item_t *btn);

/* ── buttons ───────────────────────────────────────────────────────────── */

static button_item_t btn_cncl = { .type = BTN_TEXT, .label = "Cancel", .press = btn_cancel_cb };

static buttons_page_t page_nearby = {{ NULL, NULL, NULL, NULL, &btn_cncl }};

/* ── dialog descriptor ─────────────────────────────────────────────────── */

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .rotary_cb    = NULL,
    .key_cb       = key_cb,
    .btn_page     = &page_nearby,
};

dialog_t *dialog_pota_nearby = &dialog;

/* ── state ─────────────────────────────────────────────────────────────── */

static pota_db_entry_t  *results   = NULL;
static int               results_n = 0;

/* ── async early-exit: fires after construct_cb call stack unwinds ───────
 *
 * When construct_cb can't build the list (no GPS, no DB, no parks),
 * dialog_construct has already: saved prev_page, unloaded buttons,
 * loaded page_nearby, called main_screen_keys_enable(false) — but has NOT
 * yet set nearby->run=true or current_dialog=nearby.
 *
 * We cannot call dialog_destruct() or dialog_pota_spot_return() from inside
 * construct_cb because we're still on the call stack of btn_nearby_cb which
 * lives on spot's dialog.obj — deleting it there causes a crash.
 *
 * Instead we schedule an async call. By the time it fires, dialog_construct
 * has finished: nearby->run=true, current_dialog=nearby. We can safely
 * dialog_destruct() nearby (which restores prev_page=&page_main and calls
 * main_screen_keys_enable(true)), then call dialog_pota_spot_return().
 * ────────────────────────────────────────────────────────────────────── */

static void async_abort_cb(void *arg) {
    (void)arg;
    /* nearby->run is now true, current_dialog=nearby — safe to destruct */
    dialog_destruct();
    dialog_pota_spot_return();
}

static void early_exit(const char *msg) {
    msg_schedule_text_fmt("%s", msg);
    if (results) { free(results); results = NULL; }
    results_n = 0;
    lv_async_call(async_abort_cb, NULL);
}

/* ── row click ──────────────────────────────────────────────────────────── */

static void row_click_cb(lv_event_t *e) {
    lv_obj_t   *btn  = lv_event_get_target(e);
    const char *park = (const char *)lv_obj_get_user_data(btn);
    if (!park) return;

    /* Copy before destruct_cb frees results[] */
    char park_copy[16];
    strncpy(park_copy, park, sizeof(park_copy) - 1);
    park_copy[sizeof(park_copy) - 1] = '\0';

    pota_parks_add(park_copy);
    dialog_destruct();
    dialog_pota_spot_return();
}

static void row_focus_cb(lv_event_t *e) {
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
}

/* ── cancel ─────────────────────────────────────────────────────────────── */

static void btn_cancel_cb(struct button_item_t *btn) {
    (void)btn;
    dialog_destruct();
    dialog_pota_spot_return();
}

/* ── construct ──────────────────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent) {
    double lat, lon;

    if (!gps_get_fix(&lat, &lon)) {
        early_exit("No GPS fix");
        return;
    }

    if (!pota_db_load() || !pota_db_ready()) {
        early_exit("No park database");
        return;
    }

    if (results) { free(results); results = NULL; }
    results = malloc(MAX_ROWS * sizeof(pota_db_entry_t));
    if (!results) {
        early_exit("Out of memory");
        return;
    }
    results_n = pota_db_nearest(lat, lon, results, MAX_ROWS);

    if (results_n == 0) {
        early_exit("No parks found nearby");
        return;
    }

    dialog.obj = dialog_init(parent);

    lv_obj_t *title = lv_label_create(dialog.obj);
    lv_label_set_text(title, "Nearby Parks  —  MFK: scroll   Press: select");
    lv_obj_set_style_text_color(title, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(title, &sony_22, 0);
    lv_obj_set_width(title, LIST_W);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 6);

    lv_obj_t *list = lv_list_create(dialog.obj);
    lv_obj_set_size(list, LIST_W, LIST_H);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 10, TITLE_H);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_bg_opa(list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);

    lv_obj_t *first_btn = NULL;

    for (int i = 0; i < results_n; i++) {
        char label[80];
        if (results[i].dist_km < 10.0f)
            snprintf(label, sizeof(label), "%-10s  %4.1f km  %s",
                     results[i].ref, results[i].dist_km, results[i].name);
        else if (results[i].dist_km < 1000.0f)
            snprintf(label, sizeof(label), "%-10s  %4.0f km  %s",
                     results[i].ref, results[i].dist_km, results[i].name);
        else
            snprintf(label, sizeof(label), "%-10s  >999 km  %s",
                     results[i].ref, results[i].name);

        lv_obj_t *btn = lv_list_add_btn(list, NULL, label);

        lv_obj_t *lbl = lv_obj_get_child(btn, -1);
        if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);

        lv_obj_set_style_text_font(btn, &sony_22, 0);
        lv_obj_set_style_text_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A3A5C), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E6FBF), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_ver(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_set_width(btn, LIST_W);

        lv_obj_set_user_data(btn, (void *)results[i].ref);
        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, row_focus_cb, LV_EVENT_FOCUSED, NULL);

        lv_group_add_obj(keyboard_group, btn);

        if (i == 0) first_btn = btn;
    }

    if (first_btn)
        lv_group_focus_obj(first_btn);
}

/* ── destruct ───────────────────────────────────────────────────────────── */

static void destruct_cb(void) {
    results_n = 0;
    if (results) { free(results); results = NULL; }
}

/* ── key handler ────────────────────────────────────────────────────────── */

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    if (key == LV_KEY_ESC) {
        dialog_destruct();
        dialog_pota_spot_return();
    }
}
