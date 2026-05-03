/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA Nearby dialog — GPS-sorted park list → one-press spot.
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 *
 *  UX:
 *    Opens only when GPS has a fix. Reads fix at open time. Sorts all parks
 *    by distance. Shows a rotary-scrollable list:
 *
 *      K-1234  Jasper-Pulaski  14.2 km
 *      K-4321  Kankakee River  38.1 km
 *      ...
 *
 *    Select a row → spot that park immediately (same path as manual entry).
 *    ESC / back → dismiss.
 */

#include "dialog_pota_nearby.h"

#include "dialog.h"
#include "pota_db.h"
#include "pota_spot.h"
#include "gps.h"
#include "styles.h"
#include "events.h"
#include "params/params.h"
#include "msg.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <math.h>

/* ── tunables ──────────────────────────────────────────────────────────── */

#define MAX_ROWS    200     /* max parks shown in list */
#define ROW_H       52      /* px per row */

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
    .key_cb       = key_cb,
};

dialog_t *dialog_pota_nearby = &dialog;

/* ── state ─────────────────────────────────────────────────────────────── */

static pota_db_entry_t  *results    = NULL;
static int               results_n  = 0;
static lv_obj_t         *list       = NULL;

/* ── spot a selected park ──────────────────────────────────────────────── */

static void do_spot(const char *park) {
    int32_t     freq_hz = subject_get_int(cfg_cur.fg_freq);
    const char *mode    = pota_spot_mode_str();

    msg_schedule_text_fmt("Spotting %s...", park);
    bool ok = pota_spot_wifi(park, freq_hz, mode, NULL);

    if (ok) {
        msg_schedule_text_fmt("%s spotted! Check pota.app", park);
    } else if (wifi_get_status() != WIFI_CONNECTED) {
        msg_schedule_text_fmt("No WiFi — spot failed");
    } else {
        msg_schedule_text_fmt("POTA API error — check callsign");
    }

    dialog_destruct();
}

/* ── row click callback ────────────────────────────────────────────────── */

static void row_click_cb(lv_event_t *e) {
    lv_obj_t   *btn  = lv_event_get_target(e);
    const char *park = (const char *)lv_obj_get_user_data(btn);
    if (park) do_spot(park);
}

/* ── construct ─────────────────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent) {
    double lat, lon;

    /* ── guard: need GPS fix ─────────────────────────────────────────── */
    if (!gps_get_fix(&lat, &lon)) {
        msg_schedule_text_fmt("No GPS fix");
        dialog_destruct();
        return;
    }

    /* ── guard: need DB ──────────────────────────────────────────────── */
    if (!pota_db_load() || !pota_db_ready()) {
        msg_schedule_text_fmt("No park database");
        dialog_destruct();
        return;
    }

    /* ── sort parks ──────────────────────────────────────────────────── */
    results_n = 0;
    if (results) { free(results); results = NULL; }
    results = malloc(MAX_ROWS * sizeof(pota_db_entry_t));
    if (!results) {
        msg_schedule_text_fmt("Out of memory");
        dialog_destruct();
        return;
    }
    results_n = pota_db_nearest(lat, lon, results, MAX_ROWS);

    if (results_n == 0) {
        msg_schedule_text_fmt("No parks found");
        dialog_destruct();
        return;
    }

    /* ── title label ─────────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_add_style(title, &style_label_small, 0);
    lv_label_set_text_fmt(title, "Nearby Parks  (%.4f, %.4f)", lat, lon);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    /* ── scrollable list ─────────────────────────────────────────────── */
    list = lv_list_create(parent);
    lv_obj_set_size(list, lv_obj_get_width(parent) - 16,
                    lv_obj_get_height(parent) - 40);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 8, 34);
    lv_obj_add_style(list, &style_dialog_msg, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < results_n; i++) {
        char label[48];
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
        lv_obj_add_style(btn, &style_dialog_msg, 0);
        lv_obj_set_user_data(btn, (void *)results[i].ref);
        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, NULL);
    }
}

/* ── destruct ──────────────────────────────────────────────────────────── */

static void destruct_cb(void) {
    list      = NULL;
    results_n = 0;
    if (results) { free(results); results = NULL; }
}

/* ── key handler ───────────────────────────────────────────────────────── */

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    switch (key) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        default:
            break;
    }
}
