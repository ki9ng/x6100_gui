/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot dialog
 *
 *  UI:
 *    Main screen — park history list (tap any park → spots immediately)
 *                  [New Park] [Cancel] buttons
 *    New park    — textarea_window (same pattern as dialog_callsign)
 *                  on OK → spots and closes
 *
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#include "dialog_pota_spot.h"

#include "buttons.h"
#include "cfg/cfg.h"
#include "dialog.h"
#include "events.h"
#include "keyboard.h"
#include "lvgl/lvgl.h"
#include "msg.h"
#include "params/params.h"
#include "pota_parks.h"
#include "pota_spot.h"
#include "styles.h"
#include "textarea_window.h"
#include "wifi.h"

#include <stdio.h>
#include <string.h>

/* ─── forward declarations ──────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void btn_new_park_cb(struct button_item_t *btn);
static void btn_cancel_cb(struct button_item_t *btn);

static bool textarea_ok_cb(void);
static bool textarea_cancel_cb(void);

static void do_spot(const char *park);

/* ─── state ─────────────────────────────────────────────────────────────── */

static lv_obj_t *list;
static bool      in_textarea = false;

/* ─── buttons ───────────────────────────────────────────────────────────── */

static button_item_t btn_new  = { .type = BTN_TEXT, .label = "New Park", .press = btn_new_park_cb };
static button_item_t btn_cncl = { .type = BTN_TEXT, .label = "Cancel",   .press = btn_cancel_cb  };

static buttons_page_t page_main = {{ &btn_new, NULL, NULL, NULL, &btn_cncl }};

/* ─── dialog descriptor ─────────────────────────────────────────────────── */

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .rotary_cb    = NULL,
    .key_cb       = key_cb,
};

dialog_t *dialog_pota_spot = &dialog;

/* ─── helpers ───────────────────────────────────────────────────────────── */

static const char *mode_str(void) {
    switch ((x6100_mode_t)subject_get_int(cfg_cur.mode)) {
        case x6100_mode_lsb:
        case x6100_mode_usb:      return "SSB";
        case x6100_mode_lsb_dig:
        case x6100_mode_usb_dig:  return "DATA";
        case x6100_mode_cw:
        case x6100_mode_cwr:      return "CW";
        case x6100_mode_am:       return "AM";
        case x6100_mode_nfm:      return "FM";
        default:                  return "SSB";
    }
}

static void do_spot(const char *park) {
    int32_t     freq_hz = subject_get_int(cfg_cur.fg_freq);
    const char *mode    = mode_str();

    msg_schedule_text_fmt("Spotting %s...", park);

    bool ok = pota_spot_wifi(park, freq_hz, mode, NULL);

    if (ok) {
        msg_schedule_text_fmt("%s spotted! Check pota.app", park);
    } else if (wifi_get_status() != WIFI_CONNECTED) {
        msg_schedule_text_fmt("No WiFi — spot failed");
    } else {
        msg_schedule_text_fmt("POTA API error — check callsign");
    }

    dialog_destruct(&dialog);
}

/* ─── list item click ───────────────────────────────────────────────────── */

static void list_btn_click_cb(lv_event_t *e) {
    lv_obj_t   *btn  = lv_event_get_target(e);
    const char *park = (const char *)lv_obj_get_user_data(btn);
    if (park) do_spot(park);
}

/* ─── button callbacks ──────────────────────────────────────────────────── */

static void btn_new_park_cb(struct button_item_t *btn) {
    (void)btn;
    in_textarea = true;

    /* Replace dialog with textarea_window — same pattern as dialog_callsign */
    lv_obj_del(dialog.obj);
    dialog.obj = textarea_window_open(textarea_ok_cb, textarea_cancel_cb);

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

static void btn_cancel_cb(struct button_item_t *btn) {
    (void)btn;
    dialog_destruct(&dialog);
}

/* ─── textarea callbacks ────────────────────────────────────────────────── */

static bool textarea_ok_cb(void) {
    const char *val = textarea_window_get();
    if (!val || strlen(val) == 0) {
        msg_schedule_text_fmt("Enter a park reference");
        return false;
    }

    /* Normalize to upper-case */
    char park[16];
    strncpy(park, val, sizeof(park) - 1);
    park[sizeof(park) - 1] = '\0';
    for (char *c = park; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;

    textarea_window_close();
    in_textarea = false;

    do_spot(park);
    return true;
}

static bool textarea_cancel_cb(void) {
    textarea_window_close();
    in_textarea = false;
    dialog_destruct(&dialog);
    return true;
}

/* ─── construct / destruct / key ────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    in_textarea = false;

    int n = pota_parks_count();

    /* Header label */
    lv_obj_t *title = lv_label_create(dialog.obj);
    lv_label_set_text(title, n > 0 ? "Select Park or tap New Park" : "No recent parks — tap New Park");
    lv_obj_set_style_text_color(title, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_pad_all(title, 8, 0);
    lv_obj_set_width(title, 400);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* Scrollable park list */
    list = lv_list_create(dialog.obj);
    lv_obj_set_size(list, 380, 200);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_20, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);

    for (int i = 0; i < n; i++) {
        const char *park = pota_parks_get(i);

        lv_obj_t *btn = lv_list_add_btn(list, NULL, park);
        lv_obj_set_style_text_font(btn, &sony_22, 0);
        lv_obj_set_style_text_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1C3A5E), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E5F9E), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        lv_obj_set_style_pad_ver(btn, 10, 0);

        /* Store park string pointer as user data — valid for lifetime of dialog
         * since pota_parks strings are in static storage */
        lv_obj_set_user_data(btn, (void *)park);
        lv_obj_add_event_cb(btn, list_btn_click_cb, LV_EVENT_CLICKED, NULL);

        /* Add to keyboard group so rotary encoder can scroll/select */
        lv_group_add_obj(keyboard_group, btn);
    }

    if (n == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "—");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x606060), 0);
        lv_obj_center(empty);
    }

    buttons_load_page(&page_main);
}

static void destruct_cb(void) {
    if (in_textarea) {
        textarea_window_close();
        in_textarea = false;
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            if (in_textarea) {
                textarea_window_close();
                in_textarea = false;
                dialog_destruct(&dialog);
            } else {
                dialog_destruct(&dialog);
            }
            break;

        case LV_KEY_ENTER:
            if (in_textarea) {
                textarea_ok_cb();
            }
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
