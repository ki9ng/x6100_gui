/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot dialog — WiFi path
 *
 *  Flow:
 *    1. Park # input     → [Park #] [SPOT] [CANCEL]
 *    2. Blocking POST    → lv_refr_now() shows "Posting..." before curl call
 *    3. Result screen    → success or failure + [OK]
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
#include "wifi.h"

#include <stdio.h>
#include <string.h>

/* ─── forward declarations ──────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void btn_enter_park_cb(struct button_item_t *btn);
static void btn_spot_cb(struct button_item_t *btn);
static void btn_cancel_cb(struct button_item_t *btn);
static void btn_ok_cb(struct button_item_t *btn);

static bool keyboard_ok_cb(void);
static bool keyboard_cancel_cb(void);

/* ─── state ─────────────────────────────────────────────────────────────── */

static lv_obj_t *cont_input;
static lv_obj_t *cont_progress;
static lv_obj_t *cont_result;

static lv_obj_t *label_park_val;
static lv_obj_t *label_freq_val;
static lv_obj_t *label_posting;
static lv_obj_t *label_result;
static lv_obj_t *label_result_detail;

static char park_buf[16];

/* ─── button items + pages ──────────────────────────────────────────────── */

static button_item_t btn_enter  = { .type = BTN_TEXT, .label = "Park #", .press = btn_enter_park_cb };
static button_item_t btn_spot   = { .type = BTN_TEXT, .label = "SPOT",   .press = btn_spot_cb       };
static button_item_t btn_cancel = { .type = BTN_TEXT, .label = "CANCEL", .press = btn_cancel_cb     };
static button_item_t btn_ok     = { .type = BTN_TEXT, .label = "OK",     .press = btn_ok_cb         };

static buttons_page_t page_input  = {{ &btn_enter, &btn_spot, &btn_cancel }};
static buttons_page_t page_result = {{ &btn_ok }};

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
        case x6100_mode_lsb:      return "SSB";
        case x6100_mode_usb:      return "SSB";
        case x6100_mode_lsb_dig:  return "DATA";
        case x6100_mode_usb_dig:  return "DATA";
        case x6100_mode_cw:       return "CW";
        case x6100_mode_cwr:      return "CW";
        case x6100_mode_am:       return "AM";
        case x6100_mode_nfm:      return "FM";
        default:                  return "SSB";
    }
}

static void show_only(lv_obj_t *visible) {
    lv_obj_t *all[] = { cont_input, cont_progress, cont_result };
    for (int i = 0; i < 3; i++) {
        if (all[i] == visible)
            lv_obj_clear_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ─── button callbacks ──────────────────────────────────────────────────── */

static void btn_spot_cb(struct button_item_t *btn) {
    (void)btn;

    if (strlen(park_buf) == 0) {
        msg_schedule_text_fmt("Enter a park reference first");
        return;
    }

    int32_t     freq_hz = subject_get_int(cfg_cur.fg_freq);
    const char *mode    = mode_str();

    /* Show "Posting..." and flush the display before blocking */
    show_only(cont_progress);
    char posting_buf[64];
    snprintf(posting_buf, sizeof(posting_buf),
             "Posting %s  %.3f MHz  %s",
             park_buf, freq_hz / 1e6, mode);
    lv_label_set_text(label_posting, posting_buf);
    lv_refr_now(lv_disp_get_default());

    /* Blocking HTTP POST (typically < 1 second on good WiFi) */
    bool ok = pota_spot_wifi(park_buf, freq_hz, mode, NULL);

    /* Show result */
    if (ok) {
        char res_buf[48];
        snprintf(res_buf, sizeof(res_buf),
                 LV_SYMBOL_OK "  %s spotted!", park_buf);
        lv_label_set_text(label_result, res_buf);
        lv_label_set_text(label_result_detail, "Check pota.app");
    } else if (wifi_get_status() != WIFI_CONNECTED) {
        lv_label_set_text(label_result, LV_SYMBOL_CLOSE "  No WiFi");
        lv_label_set_text(label_result_detail,
            "Connect WiFi and try again.\n"
            "(FT8/SOTAmat path: coming soon)");
    } else {
        lv_label_set_text(label_result, LV_SYMBOL_CLOSE "  API error");
        lv_label_set_text(label_result_detail,
            "POTA API returned an error.\nTry again.");
    }

    show_only(cont_result);
    buttons_load_page(&page_result);
}

static void btn_cancel_cb(struct button_item_t *btn) {
    (void)btn;
    dialog_destruct();
}

static void btn_ok_cb(struct button_item_t *btn) {
    (void)btn;
    dialog_destruct();
}

static void btn_enter_park_cb(struct button_item_t *btn) {
    (void)btn;

    textarea_window_open(keyboard_ok_cb, keyboard_cancel_cb);
    lv_obj_t *ta = textarea_window_text();
    lv_textarea_set_accepted_chars(ta,
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "-");
    lv_textarea_set_max_length(ta, 10);
    lv_textarea_set_placeholder_text(ta, "K-1234");
    if (strlen(park_buf) > 0)
        textarea_window_set(park_buf);
}

/* ─── keyboard callbacks ────────────────────────────────────────────────── */

static bool keyboard_ok_cb(void) {
    const char *val = textarea_window_get();
    if (!val || strlen(val) == 0) {
        msg_schedule_text_fmt("Park reference required");
        return false;
    }
    strncpy(park_buf, val, sizeof(park_buf) - 1);
    park_buf[sizeof(park_buf) - 1] = '\0';
    for (char *c = park_buf; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;

    lv_label_set_text(label_park_val, park_buf);
    textarea_window_close();
    return true;
}

static bool keyboard_cancel_cb(void) {
    textarea_window_close();
    return true;
}

/* ─── construct / destruct / key ────────────────────────────────────────── */

/* helper to build a full-area transparent container with centered column flex */
static lv_obj_t *make_container(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 400, 280);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER,
                             LV_FLEX_ALIGN_CENTER,
                             LV_FLEX_ALIGN_CENTER);
    return c;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    park_buf[0] = '\0';

    /* ── Input screen ── */
    cont_input = make_container(dialog.obj);

    lv_obj_t *title = lv_label_create(cont_input);
    lv_label_set_text(title, "POTA Self-Spot");
    lv_obj_set_style_text_font(title, &sony_18, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *park_lbl = lv_label_create(cont_input);
    lv_label_set_text(park_lbl, "Park:");
    lv_obj_set_style_text_color(park_lbl, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_pad_top(park_lbl, 16, 0);

    label_park_val = lv_label_create(cont_input);
    lv_label_set_text(label_park_val, "— tap Park # to set —");
    lv_obj_set_style_text_font(label_park_val, &sony_18, 0);
    lv_obj_set_style_text_color(label_park_val, lv_color_white(), 0);

    label_freq_val = lv_label_create(cont_input);
    char buf[40];
    snprintf(buf, sizeof(buf), "%.3f MHz  %s",
             subject_get_int(cfg_cur.fg_freq) / 1e6, mode_str());
    lv_label_set_text(label_freq_val, buf);
    lv_obj_set_style_text_color(label_freq_val, lv_color_hex(0x808080), 0);
    lv_obj_set_style_pad_top(label_freq_val, 8, 0);

    /* ── Progress screen ── */
    cont_progress = make_container(dialog.obj);
    lv_obj_add_flag(cont_progress, LV_OBJ_FLAG_HIDDEN);

    label_posting = lv_label_create(cont_progress);
    lv_label_set_text(label_posting, "Posting...");
    lv_obj_set_style_text_font(label_posting, &sony_18, 0);
    lv_obj_set_style_text_color(label_posting, lv_color_white(), 0);

    /* ── Result screen ── */
    cont_result = make_container(dialog.obj);
    lv_obj_add_flag(cont_result, LV_OBJ_FLAG_HIDDEN);

    label_result = lv_label_create(cont_result);
    lv_label_set_text(label_result, "");
    lv_obj_set_style_text_font(label_result, &sony_18, 0);
    lv_obj_set_style_text_color(label_result, lv_color_white(), 0);

    label_result_detail = lv_label_create(cont_result);
    lv_label_set_text(label_result_detail, "");
    lv_label_set_long_mode(label_result_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label_result_detail, 360);
    lv_obj_set_style_text_color(label_result_detail, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_align(label_result_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(label_result_detail, 12, 0);

    buttons_load_page(&page_input);
}

static void destruct_cb(void) {
    textarea_window_close();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    if (key == LV_KEY_ESC)
        dialog_destruct();
}
