/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot dialog
 *
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#include "dialog_pota_spot.h"
#include "dialog_pota_nearby.h"

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
static void btn_nearby_cb(struct button_item_t *btn);
static void btn_cancel_cb(struct button_item_t *btn);

static bool textarea_ok_cb(void);
static bool textarea_cancel_cb(void);

static void do_spot(const char *park);
static void populate_list(void);

/* ─── state ─────────────────────────────────────────────────────────────── */

static lv_obj_t        *list            = NULL;
static bool             in_textarea     = false;
static bool             in_nearby       = false;
/* Page that was active before spot was ever opened — used to restore on cancel
 * even after a nearby round-trip resets dialog->prev_page. */
static buttons_page_t  *app_prev_page   = NULL;

/* ─── buttons ───────────────────────────────────────────────────────────── */

static button_item_t btn_new    = { .type = BTN_TEXT, .label = "New Park",     .press = btn_new_park_cb };
static button_item_t btn_nearby = { .type = BTN_TEXT, .label = "Nearby Parks", .press = btn_nearby_cb   };
static button_item_t btn_cncl   = { .type = BTN_TEXT, .label = "Cancel",       .press = btn_cancel_cb   };

static buttons_page_t page_main = {{ &btn_new, &btn_nearby, NULL, NULL, &btn_cncl }};

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
    lv_obj_t   *btn  = lv_event_get_target(e);
    const char *park = (const char *)lv_obj_get_user_data(btn);
    if (park) do_spot(park);
}

static void populate_list(void) {
    lv_obj_clean(list);

    int n = pota_parks_count();

    for (int i = 0; i < n; i++) {
        const char *park = pota_parks_get(i);
        lv_obj_t   *b    = lv_list_add_btn(list, NULL, park);

        lv_obj_set_style_text_font(b, &sony_22, 0);
        lv_obj_set_style_text_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1C3A5E), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2E5F9E), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
        lv_obj_set_style_pad_ver(b, 10, 0);

        lv_obj_set_user_data(b, (void *)park);
        lv_obj_add_event_cb(b, list_btn_click_cb, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(keyboard_group, b);
    }

    if (n == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "—");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x606060), 0);
        lv_obj_center(empty);
    }
}

/* ─── button callbacks ──────────────────────────────────────────────────── */

static void btn_new_park_cb(struct button_item_t *btn) {
    (void)btn;
    in_textarea = true;
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

static void btn_nearby_cb(struct button_item_t *btn) {
    (void)btn;
    in_nearby = true;
    lv_obj_add_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
    dialog_construct(dialog_pota_nearby, lv_scr_act());
}

static void btn_cancel_cb(struct button_item_t *btn) {
    (void)btn;
    /* Restore the original app page, not whatever nearby left as prev_page */
    dialog.prev_page = app_prev_page;
    dialog_destruct();
}

/* ─── called by dialog_pota_nearby when it closes ──────────────────────── */

void dialog_pota_spot_return(void) {
    in_nearby = false;

    /* At this point:
     *   - nearby has been dialog_destruct()ed:
     *       current_dialog = NULL
     *       cur_page restored to &page_main  (was nearby->prev_page)
     *       main_screen_keys_enable(true) scheduled
     *   - spot dialog.obj still exists (was hidden, not deleted)
     *   - dialog.run is still true
     *   - app_prev_page still holds the original app page
     *
     * We need to bring spot back as if it was freshly constructed:
     *   1. Delete the stale hidden obj
     *   2. Reset run=false so dialog_construct does full setup
     *   3. Patch prev_page to app_prev_page BEFORE dialog_construct saves
     *      buttons_get_cur_page() — we do this by temporarily making
     *      buttons_get_cur_page() return app_prev_page by loading it first,
     *      but that's complex. Simpler: construct, then fix prev_page after.
     */

    /* Tear down the hidden spot obj cleanly */
    if (dialog.obj) {
        lv_obj_del(dialog.obj);
        dialog.obj = NULL;
    }
    dialog.run = false;

    /* dialog_construct will call buttons_get_cur_page() to save prev_page.
     * cur_page is currently &page_main (restored by nearby's destruct).
     * That would make prev_page=&page_main which is wrong.
     * Load app_prev_page first so dialog_construct saves the right page. */
    buttons_unload_page();
    buttons_load_page(app_prev_page);

    dialog_construct(dialog_pota_spot, lv_scr_act());
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
    in_nearby   = false;

    /* Capture the original app page once — preserved across nearby round-trips */
    if (app_prev_page == NULL) {
        app_prev_page = dialog.prev_page;
    }

    lv_obj_t *title = lv_label_create(dialog.obj);
    lv_label_set_text(title, pota_parks_count() > 0
        ? "Select Park or tap New Park"
        : "No recent parks — tap New Park");
    lv_obj_set_style_text_color(title, lv_color_hex(0xC0C0C0), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    list = lv_list_create(dialog.obj);
    lv_obj_set_size(list, 380, 200);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 36);
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
    /* Clear app_prev_page only on a true close, not a nearby round-trip */
    if (!in_nearby) {
        app_prev_page = NULL;
    }
    list = NULL;
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            if (in_textarea) {
                textarea_window_close();
                in_textarea = false;
            }
            dialog.prev_page = app_prev_page;
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
