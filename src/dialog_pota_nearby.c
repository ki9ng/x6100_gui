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

#define MAX_ROWS    20
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

static pota_db_entry_t  results[MAX_ROWS];   /* static — no malloc/free needed */
static int              results_n = 0;

/* Park refs copied at populate time so async callbacks never touch results[] */
static char             park_refs[MAX_ROWS][POTA_DB_REF_LEN];

/* Pending selection for async_select_cb */
static char             selected_park[POTA_DB_REF_LEN];

/* Pending cancel/abort — set true then async fires */
static bool             pending_cancel = false;

/* ── async callbacks ────────────────────────────────────────────────────── */

/*
 * All three async callbacks share the same structure:
 *   1. dialog_destruct() — safe because nearby->run=true by now
 *   2. dialog_pota_spot_return() — rebuilds spot dialog
 * The difference is whether we commit a park selection first.
 */

static void async_select_cb(void *arg) {
    (void)arg;
    LV_LOG_USER("async_select_cb: selecting park '%s'", selected_park);
    pota_parks_add(selected_park);
    dialog_destruct();
    dialog_pota_spot_return();
}

static void async_cancel_cb(void *arg) {
    (void)arg;
    LV_LOG_USER("async_cancel_cb: enter");
    dialog_destruct();
    dialog_pota_spot_return();
}

/* ── early-exit helper ──────────────────────────────────────────────────── */
/*
 * Called from construct_cb before dialog_init() — dialog.obj is still NULL,
 * nearby->run is still false, current_dialog is still spot.
 * We CANNOT call dialog_destruct() or dialog_pota_spot_return() here.
 * Schedule async_cancel_cb; by the time it fires dialog_construct() has
 * finished: nearby->run=true, current_dialog=nearby, safe to destruct.
 */
static void early_exit(const char *msg) {
    LV_LOG_USER("nearby early_exit: %s", msg);
    msg_schedule_text_fmt("%s", msg);
    results_n = 0;
    lv_async_call(async_cancel_cb, NULL);
}

/* ── row events ─────────────────────────────────────────────────────────── */

static void row_click_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= results_n) return;

    /* Copy ref into selected_park before async fires */
    strncpy(selected_park, park_refs[idx], sizeof(selected_park) - 1);
    selected_park[sizeof(selected_park) - 1] = '\0';

    /* Defer: calling dialog_destruct() here (inside lv_event_send) would
     * delete the button that fired this event — use-after-free crash. */
    lv_async_call(async_select_cb, NULL);
}

static void row_focus_cb(lv_event_t *e) {
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
}

/* ── cancel / ESC ───────────────────────────────────────────────────────── */
/*
 * Both cancel paths defer via async — calling dialog_destruct() synchronously
 * from a button press handler or key event would delete dialog.obj (parent
 * of the button) while still inside LVGL's event dispatch — use-after-free.
 */

static void btn_cancel_cb(struct button_item_t *btn) {
    (void)btn;
    LV_LOG_USER("btn_cancel_cb: scheduling async_cancel");
    lv_async_call(async_cancel_cb, NULL);
}

/* ── construct ──────────────────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent) {
    double lat, lon;

    LV_LOG_USER("nearby construct_cb: enter");

    if (!gps_get_fix(&lat, &lon)) {
        early_exit("No GPS fix");
        return;
    }
    LV_LOG_USER("nearby construct_cb: gps ok lat=%.4f lon=%.4f", lat, lon);

    if (!pota_db_load() || !pota_db_ready()) {
        early_exit("No park database");
        return;
    }
    LV_LOG_USER("nearby construct_cb: db ready, %d parks total", pota_db_count());

    results_n = pota_db_nearest(lat, lon, results, MAX_ROWS);
    LV_LOG_USER("nearby construct_cb: pota_db_nearest returned %d parks", results_n);

    if (results_n == 0) {
        early_exit("No parks found nearby");
        return;
    }

    /* Copy park refs into our static array — buttons store index, not pointer */
    for (int i = 0; i < results_n; i++) {
        strncpy(park_refs[i], results[i].ref, POTA_DB_REF_LEN - 1);
        park_refs[i][POTA_DB_REF_LEN - 1] = '\0';
    }

    /* ── build UI ─────────────────────────────────────────────────────── */

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
                     park_refs[i], results[i].dist_km, results[i].name);
        else if (results[i].dist_km < 1000.0f)
            snprintf(label, sizeof(label), "%-10s  %4.0f km  %s",
                     park_refs[i], results[i].dist_km, results[i].name);
        else
            snprintf(label, sizeof(label), "%-10s  >999 km  %s",
                     park_refs[i], results[i].name);

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

        /* Store index, not pointer — park_refs[] is static and safe */
        lv_obj_set_user_data(btn, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, row_focus_cb, LV_EVENT_FOCUSED, NULL);

        lv_group_add_obj(keyboard_group, btn);

        if (i == 0) first_btn = btn;
    }

    if (first_btn)
        lv_group_focus_obj(first_btn);

    LV_LOG_USER("nearby construct_cb: UI built, %d rows", results_n);
}

/* ── destruct ───────────────────────────────────────────────────────────── */

static void destruct_cb(void) {
    LV_LOG_USER("nearby destruct_cb: clearing %d results", results_n);
    results_n = 0;
    /* results[] is static — no free needed */
}

/* ── key handler ────────────────────────────────────────────────────────── */

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    if (key == LV_KEY_ESC) {
        LV_LOG_USER("key_cb: ESC — scheduling async_cancel");
        lv_async_call(async_cancel_cb, NULL);
    }
}
