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

#include "dialog.h"
#include "keyboard.h"
#include "pota_db.h"
#include "pota_parks.h"
#include "gps.h"
#include "msg.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── tunables ──────────────────────────────────────────────────────────── */

#define MAX_ROWS    200

/* ── forward declarations ──────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

/* ── dialog descriptor ─────────────────────────────────────────────────── */

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .rotary_cb    = NULL,
    .key_cb       = key_cb,
};

dialog_t *dialog_pota_nearby = &dialog;

/* ── state ─────────────────────────────────────────────────────────────── */

static pota_db_entry_t  *results   = NULL;
static int               results_n = 0;

/* ── row click: add park to history, return to spot dialog ─────────────── */

static void row_click_cb(lv_event_t *e) {
    lv_obj_t   *btn  = lv_event_get_target(e);
    const char *park = (const char *)lv_obj_get_user_data(btn);
    if (!park) return;

    pota_parks_add(park);
    dialog_destruct();
    dialog_pota_spot_return();
}

/* ── scroll focused row into view ──────────────────────────────────────── */

static void row_focus_cb(lv_event_t *e) {
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_ON);
}

/* ── construct ─────────────────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent) {
    double lat, lon;

    if (!gps_get_fix(&lat, &lon)) {
        msg_schedule_text_fmt("No GPS fix");
        dialog_pota_spot_return();
        return;
    }

    if (!pota_db_load() || !pota_db_ready()) {
        msg_schedule_text_fmt("No park database");
        dialog_pota_spot_return();
        return;
    }

    if (results) { free(results); results = NULL; }
    results = malloc(MAX_ROWS * sizeof(pota_db_entry_t));
    if (!results) {
        msg_schedule_text_fmt("Out of memory");
        dialog_pota_spot_return();
        return;
    }
    results_n = pota_db_nearest(lat, lon, results, MAX_ROWS);

    if (results_n == 0) {
        msg_schedule_text_fmt("No parks found");
        dialog_pota_spot_return();
        return;
    }

    /* Create the dialog container — dialog_destruct() calls lv_obj_del on
     * dialog.obj, which destroys all children. Without this every widget
     * would be parented to lv_scr_act() and never cleaned up. */
    dialog.obj = dialog_init(parent);

    /* ── title ────────────────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(dialog.obj);
    lv_label_set_text_fmt(title, "Nearby Parks  (%.4f, %.4f)", lat, lon);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    lv_obj_t *hint = lv_label_create(dialog.obj);
    lv_label_set_text(hint, "MFK: scroll  \xE2\x80\xA2  Press: select");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x808080), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 6);

    /* ── scrollable list ──────────────────────────────────────────────── */
    lv_obj_t *list = lv_list_create(dialog.obj);
    lv_obj_set_size(list, 460, 220);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 8, 34);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lv_obj_t *first_btn = NULL;

    for (int i = 0; i < results_n; i++) {
        char label[64];
        if (results[i].dist_km < 10.0f)
            snprintf(label, sizeof(label), "%-9s %.1f km  %s",
                     results[i].ref, results[i].dist_km, results[i].name);
        else if (results[i].dist_km < 1000.0f)
            snprintf(label, sizeof(label), "%-9s %.0f km  %s",
                     results[i].ref, results[i].dist_km, results[i].name);
        else
            snprintf(label, sizeof(label), "%-9s >999 km  %s",
                     results[i].ref, results[i].name);

        lv_obj_t *btn = lv_list_add_btn(list, NULL, label);
        lv_obj_set_user_data(btn, (void *)results[i].ref);
        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, row_focus_cb, LV_EVENT_FOCUSED, NULL);

        lv_group_add_obj(keyboard_group, btn);

        if (i == 0) first_btn = btn;
    }

    if (first_btn)
        lv_group_focus_obj(first_btn);
}

/* ── destruct ──────────────────────────────────────────────────────────── */

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
