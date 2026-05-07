/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot dialog — unified picker for recent + nearby parks.
 *
 *  KI9NG — ki9ng/x6100_gui feature/pota-nearby-unified
 *
 *  Design rationale: see wiki page radio/x6100-pota-nearby-ux-plan.md.
 *  Previously this was a two-dialog setup (spot + nearby) with hide-not-destruct
 *  round-trip that produced a long string of crashes. The unified list keeps
 *  everything in this one dialog: RECENT section from pota_parks history, then
 *  a NEARBY section from pota_db_nearest() when GPS is locked. Section headers
 *  use lv_list_add_text() which is not focusable, so the MFK encoder naturally
 *  skips past them.
 */

#include "dialog_pota_spot.h"

#include "buttons.h"
#include "cfg/cfg.h"
#include "dialog.h"
#include "events.h"
#include "gps.h"
#include "keyboard.h"
#include "lvgl/lvgl.h"
#include "msg.h"
#include "params/params.h"
#include "pota_db.h"
#include "pota_parks.h"
#include "pota_spot.h"
#include "styles.h"
#include "textarea_window.h"
#include "wifi.h"

#include <stdio.h>
#include <string.h>

/* ─── tunables ──────────────────────────────────────────────────────────── */

#define MAX_NEARBY      5      /* how many nearest parks to show */
#define LIST_W          776    /* dialog is 796px wide */
#define LIST_H          280    /* leave room for title above */
#define TITLE_H         32

/* ─── forward declarations ──────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void btn_new_park_cb(struct button_item_t *btn);
static void btn_refresh_cb(struct button_item_t *btn);
static void btn_cancel_cb(struct button_item_t *btn);

static bool textarea_ok_cb(void);
static bool textarea_cancel_cb(void);

static void do_spot(const char *park);
static void populate_list(void);
static void list_btn_click_cb(lv_event_t *e);

/* ─── state ─────────────────────────────────────────────────────────────── */

static lv_obj_t *list        = NULL;
static lv_obj_t *title_lbl   = NULL;
static bool      in_textarea = false;

/* Static park-ref storage so list-button user_data remains valid for the
 * lifetime of the dialog. We store at most POTA_PARKS_MAX recent + MAX_NEARBY
 * nearby = 15 entries. */
#define MAX_REFS (POTA_PARKS_MAX + MAX_NEARBY)
static char park_refs[MAX_REFS][POTA_DB_REF_LEN];
static int  park_refs_n = 0;

/* ─── buttons ───────────────────────────────────────────────────────────── */

static button_item_t btn_new     = { .type = BTN_TEXT, .label = "New Park",        .press = btn_new_park_cb };
static button_item_t btn_refresh = { .type = BTN_TEXT, .label = "Refresh\nNearby", .press = btn_refresh_cb  };
static button_item_t btn_cncl    = { .type = BTN_TEXT, .label = "Cancel",          .press = btn_cancel_cb   };

static buttons_page_t page_main = {{ &btn_new, &btn_refresh, NULL, NULL, &btn_cncl }};

/* ─── dialog descriptor ─────────────────────────────────────────────────── */

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .rotary_cb    = NULL,
    .key_cb       = key_cb,
    .btn_page     = &page_main,
};

dialog_t *dialog_pota_spot = &dialog;

/* ─── spot helper ───────────────────────────────────────────────────────── */

static void do_spot(const char *park) {
    int32_t     freq_hz = subject_get_int(cfg_cur.fg_freq);
    const char *mode    = pota_spot_mode_str();

    msg_schedule_text_fmt("Spotting %s...", park);

    bool ok = pota_spot_wifi(park, freq_hz, mode, NULL);

    if (ok)
        msg_schedule_text_fmt("%s spotted! Check pota.app", park);
    else if (wifi_get_status() != WIFI_CONNECTED)
        msg_schedule_text_fmt("No WiFi — spot failed");
    else
        msg_schedule_text_fmt("POTA API error — check callsign");

    dialog_destruct();
}

/* ─── list population ───────────────────────────────────────────────────── */

static void list_btn_click_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= park_refs_n) return;

    /* Add nearby selections to recent history before spotting. pota_parks_add
     * is a no-op if the park is already at the top of recent. */
    pota_parks_add(park_refs[idx]);

    do_spot(park_refs[idx]);
}

static lv_obj_t *add_park_row(lv_obj_t *parent, const char *label_text, int ref_idx) {
    lv_obj_t *btn = lv_list_add_btn(parent, NULL, label_text);

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

    lv_obj_set_user_data(btn, (void *)(intptr_t)ref_idx);
    lv_obj_add_event_cb(btn, list_btn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(keyboard_group, btn);

    return btn;
}

static void add_section_header(lv_obj_t *parent, const char *text) {
    /* lv_list_add_text creates a non-button label child of the list. It is
     * NOT added to keyboard_group, so MFK navigation skips it automatically. */
    lv_obj_t *hdr = lv_list_add_text(parent, text);
    lv_obj_set_style_text_font(hdr, &sony_22, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_ver(hdr, 4, 0);
    lv_obj_set_style_pad_hor(hdr, 8, 0);
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_CLIP);
}

static void populate_list(void) {
    if (!list) return;

    lv_obj_clean(list);
    park_refs_n = 0;

    lv_obj_t *first_btn = NULL;

    /* ── RECENT section ───────────────────────────────────────────────── */
    int recent_n = pota_parks_count();
    if (recent_n > 0) {
        add_section_header(list, "── RECENT ──");
        for (int i = 0; i < recent_n && park_refs_n < MAX_REFS; i++) {
            const char *park = pota_parks_get(i);
            if (!park) continue;

            strncpy(park_refs[park_refs_n], park, POTA_DB_REF_LEN - 1);
            park_refs[park_refs_n][POTA_DB_REF_LEN - 1] = '\0';

            lv_obj_t *btn = add_park_row(list, park, park_refs_n);
            if (!first_btn) first_btn = btn;

            park_refs_n++;
        }
    }

    /* ── NEARBY section ───────────────────────────────────────────────── */
    double lat, lon;
    if (gps_get_fix(&lat, &lon) && pota_db_load() && pota_db_ready()) {
        static pota_db_entry_t nearby[MAX_NEARBY];
        int nearby_n = pota_db_nearest(lat, lon, nearby, MAX_NEARBY);

        if (nearby_n > 0) {
            add_section_header(list, "── NEARBY ──");

            for (int i = 0; i < nearby_n && park_refs_n < MAX_REFS; i++) {
                strncpy(park_refs[park_refs_n], nearby[i].ref, POTA_DB_REF_LEN - 1);
                park_refs[park_refs_n][POTA_DB_REF_LEN - 1] = '\0';

                char label[80];
                if (nearby[i].dist_km < 10.0f)
                    snprintf(label, sizeof(label), "%-10s  %4.1f km  %s",
                             nearby[i].ref, nearby[i].dist_km, nearby[i].name);
                else if (nearby[i].dist_km < 1000.0f)
                    snprintf(label, sizeof(label), "%-10s  %4.0f km  %s",
                             nearby[i].ref, nearby[i].dist_km, nearby[i].name);
                else
                    snprintf(label, sizeof(label), "%-10s  >999 km  %s",
                             nearby[i].ref, nearby[i].name);

                lv_obj_t *btn = add_park_row(list, label, park_refs_n);
                if (!first_btn) first_btn = btn;

                park_refs_n++;
            }
        }
    }

    /* ── empty state ──────────────────────────────────────────────────── */
    if (park_refs_n == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "Tap New Park to enter a reference");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x808080), 0);
        lv_obj_set_style_text_font(empty, &sony_22, 0);
        lv_obj_center(empty);
    }

    /* Focus the first selectable row so MFK works immediately */
    if (first_btn) {
        lv_group_focus_obj(first_btn);
    }

    /* Update title with current state */
    if (title_lbl) {
        if (park_refs_n > 0)
            lv_label_set_text(title_lbl, "MFK: scroll   Press: spot");
        else
            lv_label_set_text(title_lbl, "No parks — tap New Park");
    }
}

/* ─── button callbacks ──────────────────────────────────────────────────── */

static void btn_new_park_cb(struct button_item_t *btn) {
    (void)btn;
    in_textarea = true;
    if (dialog.obj)
        lv_obj_add_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
    textarea_window_open(textarea_ok_cb, textarea_cancel_cb);

    lv_obj_t *ta = textarea_window_text();
    lv_textarea_set_accepted_chars(ta,
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "-");
    lv_textarea_set_max_length(ta, 10);
    lv_textarea_set_placeholder_text(ta, "US-0765");
    lv_obj_add_event_cb(ta, key_cb, LV_EVENT_KEY, NULL);
}

static void btn_refresh_cb(struct button_item_t *btn) {
    (void)btn;
    populate_list();
    msg_schedule_text_fmt("Refreshed");
}

static void btn_cancel_cb(struct button_item_t *btn) {
    (void)btn;
    dialog_destruct();
}

/* ─── textarea callbacks ────────────────────────────────────────────────── */

static bool textarea_ok_cb(void) {
    const char *val = textarea_window_get();
    if (!val || strlen(val) == 0) {
        msg_schedule_text_fmt("Enter a park reference");
        return false;
    }

    char park[16];
    strncpy(park, val, sizeof(park) - 1);
    park[sizeof(park) - 1] = '\0';
    for (char *c = park; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;

    in_textarea = false;
    pota_parks_add(park);
    do_spot(park);
    return true;
}

static bool textarea_cancel_cb(void) {
    in_textarea = false;
    if (dialog.obj)
        lv_obj_clear_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
    return true;
}

/* ─── construct / destruct / key ────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent) {
    dialog.obj  = dialog_init(parent);
    in_textarea = false;

    title_lbl = lv_label_create(dialog.obj);
    lv_label_set_text(title_lbl, "Loading...");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(title_lbl, &sony_22, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 6);

    list = lv_list_create(dialog.obj);
    lv_obj_set_size(list, LIST_W, LIST_H);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, TITLE_H);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_bg_opa(list, LV_OPA_20, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);

    populate_list();
}

static void destruct_cb(void) {
    if (in_textarea) {
        textarea_window_close();
        in_textarea = false;
    }
    list        = NULL;
    title_lbl   = NULL;
    park_refs_n = 0;
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            if (in_textarea) {
                textarea_window_close();
                in_textarea = false;
            }
            dialog_destruct();
            break;

        case LV_KEY_ENTER:
            if (in_textarea)
                textarea_ok_cb();
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
