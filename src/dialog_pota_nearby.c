/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA Nearby dialog — GPS-sorted park list → MFK knob select → press to spot.
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 *
 *  UX:
 *    Opens only when GPS has a fix. Reads fix at open time. Sorts all parks
 *    by distance. Shows an MFK-navigable list:
 *
 *      K-1234  Jasper-Pulaski  14.2 km   ← focused row
 *      K-4321  Kankakee River  38.1 km
 *      ...
 *
 *    Turn MFK knob → move focus up/down (LVGL encoder nav via keyboard_group).
 *    Press MFK knob → LV_EVENT_CLICKED fires on focused btn → spot.
 *    ESC / back → dismiss.
 *
 *  Input routing:
 *    MFK is encoder_init("/dev/input/event3") registered as LV_INDEV_TYPE_ENCODER
 *    on keyboard_group. Buttons added to that group receive enc_diff nav and
 *    press-as-click automatically — no rotary_cb needed.
 */

#include "dialog_pota_nearby.h"

#include "dialog.h"
#include "keyboard.h"
#include "pota_db.h"
#include "pota_spot.h"
#include "wifi.h"
#include "gps.h"
#include "styles.h"
#include "events.h"
#include "params/params.h"
#include "msg.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ── tunables ──────────────────────────────────────────────────────────── */

#define MAX_ROWS    200     /* max parks shown in list */

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
    .rotary_cb    = NULL,   /* VFO knob — not used here */
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

/* ── row click callback (fires on MFK press when btn is focused) ────────── */

static void row_click_cb(lv_event_t *e) {
    lv_obj_t   *btn  = lv_event_get_target(e);
    const char *park = (const char *)lv_obj_get_user_data(btn);
    if (park) do_spot(park);
}

/* ── scroll list to keep focused child visible ──────────────────────────── */

static void row_focus_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_scroll_to_view(btn, LV_ANIM_ON);
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
    lv_label_set_text_fmt(title, "Nearby Parks  (%.4f, %.4f)", lat, lon);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);

    /* ── hint ────────────────────────────────────────────────────────── */
    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, "MFK: scroll  •  Press: spot");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x808080), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -8, 6);

    /* ── scrollable list ─────────────────────────────────────────────── */
    list = lv_list_create(parent);
    lv_obj_set_size(list, lv_obj_get_width(parent) - 16,
                    lv_obj_get_height(parent) - 40);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 8, 34);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lv_obj_t *first_btn = NULL;

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
        /* Store a pointer into the results array — valid until destruct_cb frees it */
        lv_obj_set_user_data(btn, (void *)results[i].ref);
        lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, row_focus_cb, LV_EVENT_FOCUSED, NULL);

        /* Register with MFK encoder group so knob turn navigates and press clicks */
        lv_group_add_obj(keyboard_group, btn);

        if (i == 0) first_btn = btn;
    }

    /* Focus the first row so the knob is ready immediately */
    if (first_btn)
        lv_group_focus_obj(first_btn);
}

/* ── destruct ──────────────────────────────────────────────────────────── */

static void destruct_cb(void) {
    list      = NULL;
    results_n = 0;
    if (results) { free(results); results = NULL; }
}

/* ── key handler ────────────────────────────────────────────────────────── */

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
