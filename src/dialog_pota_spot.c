/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot dialog — unified picker for recent + nearby parks,
 *  followed by a method-of-spotting picker (WiFi or JS8Call), and (if JS8)
 *  an optional band picker with ATU tune.
 *
 *  KI9NG — ki9ng/x6100_gui feature/pota-nearby-unified
 *
 *  Design rationale: see wiki page radio/x6100-pota-nearby-ux-plan.md and
 *  radio/js8call-pota-spot.md.
 *
 *  Architecture: a single dialog_t that walks through up to four view states.
 *  Each state-transition tears down all child widgets of the dialog body and
 *  rebuilds the relevant ones — the outer dialog.obj container is stable for
 *  the entire lifetime of the dialog. This avoids the hide-and-reuse traps
 *  that bit the original two-dialog setup.
 *
 *    VIEW_LIST     — RECENT + NEARBY park picker (list of clickable rows)
 *    VIEW_METHOD   — "Spot US-0765 via..." with [WiFi] [JS8Call] [Cancel]
 *    VIEW_BAND     — JS8 band picker: 80/40/30/20/17/15/12/10m, [ATU tune?] [Send] [Back]
 *    VIEW_TEXTAREA — virtual keyboard for "New Park" manual entry (existing)
 *
 *  The JS8 send path uses libx6100js8 (see projects/x6100-js8-engine) to
 *  encode the spot as a multi-frame @APRSIS CMD :APSPOT directed message,
 *  then transmits one JS8 Normal frame per 15-sec slot boundary using the
 *  same per-frame PTT pattern dialog_ft8.c uses for FT8.
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
#include "radio.h"
#include "styles.h"
#include "textarea_window.h"
#include "wifi.h"

#include "audio.h"
#include <aether_radio/x6100_control/control.h>
#include <aether_radio/x6100_control/low/control.h>
#include <x6100js8/x6100js8.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ─── tunables ──────────────────────────────────────────────────────────── */

#define MAX_NEARBY      5      /* how many nearest parks to show */
#define LIST_W          776    /* dialog body width */
#define BODY_H          280    /* available vertical space below the title */
#define TITLE_H         32

/* ─── JS8 band table ────────────────────────────────────────────────────── */

/* USB dial frequency for JS8 Normal calling, audio offset 1500 Hz.
 * Source: radio/js8call-pota-spot.md and the JS8Call community defaults. */
typedef struct {
    const char *label;       /* "20m" */
    uint32_t    dial_hz;     /* 14078000 */
} js8_band_t;

static const js8_band_t js8_bands[] = {
    { "80m",  3578000 },
    { "40m",  7078000 },
    { "30m", 10130000 },
    { "20m", 14078000 },
    { "17m", 18104000 },
    { "15m", 21078000 },
    { "12m", 24922000 },
    { "10m", 28078000 },
};
#define JS8_BANDS_N ((int)(sizeof(js8_bands) / sizeof(js8_bands[0])))

/* ─── view states ───────────────────────────────────────────────────────── */

typedef enum {
    VIEW_LIST,
    VIEW_METHOD,
    VIEW_BAND,
} view_state_t;

/* ─── forward declarations ──────────────────────────────────────────────── */

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void btn_new_park_cb(struct button_item_t *btn);
static void btn_refresh_cb(struct button_item_t *btn);
static void btn_cancel_cb(struct button_item_t *btn);
static void btn_back_cb(struct button_item_t *btn);

static bool textarea_ok_cb(void);
static bool textarea_cancel_cb(void);

static void show_list(void);
static void show_method(const char *park);
static void show_band(void);

static void wifi_do_spot(const char *park);
static void js8_do_spot(const char *park, uint32_t dial_hz, bool tune_atu);

static void list_btn_click_cb(lv_event_t *e);
static void method_wifi_btn_cb(lv_event_t *e);
static void method_js8_btn_cb(lv_event_t *e);
static void band_btn_cb(lv_event_t *e);
static const char *atu_label_getter(void);
static void atu_toggle_cb(struct button_item_t *btn);
static void send_js8_btn_cb(lv_event_t *e);

/* ─── state ─────────────────────────────────────────────────────────────── */

static view_state_t view = VIEW_LIST;
static lv_obj_t   *body  = NULL;   /* container holding view-specific children */
static lv_obj_t   *title_lbl = NULL;
static bool        in_textarea = false;

/* The currently-selected park, valid in VIEW_METHOD and VIEW_BAND. */
static char selected_park[POTA_DB_REF_LEN] = "";

/* User's JS8 band + ATU choice in VIEW_BAND. -1 = none selected. */
static int  js8_band_idx = -1;
static bool js8_tune_atu = false;
static lv_obj_t *band_btns[JS8_BANDS_N] = {0};

/* Static park-ref storage so list-button user_data remains valid for the
 * lifetime of the dialog. We store at most POTA_PARKS_MAX recent + MAX_NEARBY
 * nearby = 15 entries. */
#define MAX_REFS (POTA_PARKS_MAX + MAX_NEARBY)
static char park_refs[MAX_REFS][POTA_DB_REF_LEN];
static int  park_refs_n = 0;

/* ─── footer-button definitions (top-of-screen function row) ────────────── */
/* The footer buttons depend on which view we're in. The button table is
 * swapped via buttons_load_page() during view transitions. */

static button_item_t btn_new     = { .type = BTN_TEXT, .label = "New Park",        .press = btn_new_park_cb };
static button_item_t btn_refresh = { .type = BTN_TEXT, .label = "Refresh\nNearby", .press = btn_refresh_cb  };
static button_item_t btn_cncl    = { .type = BTN_TEXT, .label = "Cancel",          .press = btn_cancel_cb   };
static button_item_t btn_back    = { .type = BTN_TEXT, .label = "Back",            .press = btn_back_cb     };
static button_item_t btn_atu     = { .type = BTN_TEXT_FN, .label_fn = atu_label_getter, .press = atu_toggle_cb };

static buttons_page_t page_list   = {{ &btn_new,  &btn_refresh, NULL, NULL, &btn_cncl }};
static buttons_page_t page_method = {{ &btn_back, NULL,         NULL, NULL, &btn_cncl }};
static buttons_page_t page_band   = {{ &btn_back, &btn_atu,     NULL, NULL, &btn_cncl }};

/* ─── dialog descriptor ─────────────────────────────────────────────────── */

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .audio_cb     = NULL,
    .rotary_cb    = NULL,
    .key_cb       = key_cb,
    .btn_page     = &page_list,
};

dialog_t *dialog_pota_spot = &dialog;

/* ─── spot helpers ──────────────────────────────────────────────────────── */

static void wifi_do_spot(const char *park) {
    int32_t     freq_hz = subject_get_int(cfg_cur.fg_freq);
    const char *mode    = pota_spot_mode_str();

    msg_schedule_text_fmt("Spotting %s via WiFi...", park);

    bool ok = pota_spot_wifi(park, freq_hz, mode, NULL);

    if (ok)
        msg_schedule_text_fmt("%s spotted! Check pota.app", park);
    else if (wifi_get_status() != WIFI_CONNECTED)
        msg_schedule_text_fmt("No WiFi — spot failed");
    else
        msg_schedule_text_fmt("POTA API error — check callsign");

    dialog_destruct();
}

/* ─── JS8 spot helpers ──────────────────────────────────────────────────── */

/*
 * Resample int16 PCM from src_rate to dst_rate using linear interpolation.
 * Allocates a new buffer; caller must free(). Returns NULL on alloc failure.
 *
 * For 48000 → 44100 (ratio 160:147) linear interp introduces aliasing only
 * near Nyquist, well outside JS8's narrow tone band (1500 Hz ± ~25 Hz). The
 * audio is single-tone FSK so the perceptual quality is fine.
 */
static int16_t *js8_resample_linear(const int16_t *in, size_t in_n,
                                    int src_rate, int dst_rate,
                                    size_t *out_n)
{
    if (src_rate == dst_rate) {
        int16_t *buf = malloc(in_n * sizeof(int16_t));
        if (!buf) return NULL;
        memcpy(buf, in, in_n * sizeof(int16_t));
        *out_n = in_n;
        return buf;
    }
    /* Output length: round-down. We're conservative — drop the last
     * partial sample rather than read off the end. */
    size_t n_out = (size_t)((double)(in_n - 1) * dst_rate / src_rate);
    int16_t *out = malloc(n_out * sizeof(int16_t));
    if (!out) return NULL;

    double step = (double)src_rate / dst_rate;
    double pos  = 0.0;

    for (size_t wi = 0; wi < n_out; wi++) {
        size_t i0 = (size_t)pos;
        if (i0 + 1 >= in_n) { n_out = wi; break; }
        double frac = pos - (double)i0;
        out[wi] = (int16_t)lround(in[i0] * (1.0 - frac) + in[i0 + 1] * frac);
        pos += step;
    }
    *out_n = n_out;
    return out;
}

/*
 * Slot-align before transmitting the next JS8 Normal frame.
 *
 * JS8 Normal frames must start at 0/15/30/45 sec past the UTC minute. JS8Call's
 * scheduler (mainwindow.cpp::guiUpdate) implements a "late-start window": if
 * the call lands within `(dead_air - txDelay) = (2.36 - 0.2) = 2.16 sec` of
 * a slot boundary, TX fires immediately rather than waiting for the next one.
 * The X6100JS8_LATE_WINDOW_MS constant (2160 ms) in libx6100js8 matches this.
 *
 * Why the wide window matters here (see wiki radio/js8call-pota-spot.md
 * "Firmware Audit", Bugs 3 + 4):
 *
 *   - Bug 3: operator presses Send 0.5 sec into a slot. With a 0.1 sec window,
 *     we'd wait 14.5 sec unnecessarily. With 2.16 sec, we fire immediately.
 *
 *   - Bug 4 (the alternating-slots bug): audio_play_wait() on PulseAudio may
 *     return up to ~2 sec after the audio actually finishes playing. By the
 *     time js8_wait_for_next_slot() runs at the top of the next frame's loop
 *     iteration, into_slot is e.g. 0.14 sec — well past the old 0.1 sec
 *     window — so the old code would sleep 14.86 sec, skipping a slot. With
 *     the 2.16 sec window the late return is absorbed and we fire immediately
 *     into the slot we just crossed into.
 *
 * Same coarse-sleep + tight-busy-wait two-phase strategy as before.
 */
static void js8_wait_for_next_slot(void) {
    /* JS8Call lateThreshold: dead_air (2.36s) minus txDelay (0.2s) = 2.16s. */
    const float LATE_WINDOW = (float)X6100JS8_LATE_WINDOW_MS / 1000.0f;
    const float SLOT_PERIOD = (float)X6100JS8_SLOT_PERIOD_MS / 1000.0f;  /* 15.0 */

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    double sec_full = (double)now.tv_sec + (double)now.tv_nsec / 1.0e9;
    float  into_slot = (float)fmod(sec_full, (double)SLOT_PERIOD);
    float  wait_s    = SLOT_PERIOD - into_slot;

    /* Immediate-fire window: anywhere from boundary up to LATE_WINDOW (2.16s)
     * past a boundary fires now. Also fire if we're within ~25 ms of the
     * NEXT boundary (saves a no-op coarse sleep). */
    if (into_slot < LATE_WINDOW)        return;
    if (wait_s    < 0.025f)             return;

    /* Coarse sleep to ~25 ms before the boundary. */
    float coarse_s = wait_s - 0.025f;
    if (coarse_s > 0.0f) {
        struct timespec rqt;
        rqt.tv_sec  = (time_t)coarse_s;
        rqt.tv_nsec = (long)((coarse_s - (float)rqt.tv_sec) * 1.0e9f);
        nanosleep(&rqt, NULL);
    }

    /* Tight wait for the last few ms — busy-wait, accuracy matters here. */
    while (1) {
        clock_gettime(CLOCK_REALTIME, &now);
        sec_full  = (double)now.tv_sec + (double)now.tv_nsec / 1.0e9;
        into_slot = (float)fmod(sec_full, (double)SLOT_PERIOD);
        if (into_slot < 0.025f || into_slot > (SLOT_PERIOD - 0.025f)) break;
        usleep(1000);
    }
}

/*
 * Wait for an ATU tune cycle to complete. radio_start_atu() just flips the
 * radio state machine into RADIO_ATU_START; the actual command goes out on
 * the next radio_tick(), the state progresses _START -> _WAIT -> _RUN, and
 * eventually returns to RADIO_RX when the matching network has settled.
 *
 * Tune duration is highly variable on this radio — anywhere from ~1 sec
 * for an easy match up to 30 sec for a difficult one. We poll the state
 * up to a 35 sec hard cap.
 *
 * Returns true if tune completed, false on timeout.
 */
static bool js8_wait_for_atu_done(int max_seconds) {
    /* Phase 1: wait for state to LEAVE RADIO_RX. Confirms the start cmd
     * actually fired. Should happen within ~100 ms. */
    for (int i = 0; i < 20; i++) {
        if (radio_get_state() != RADIO_RX) break;
        usleep(50000);
    }

    /* Phase 2: wait for state to RETURN to RADIO_RX. */
    for (int i = 0; i < max_seconds * 10; i++) {
        radio_state_t s = radio_get_state();
        if (s == RADIO_RX) return true;
        usleep(100000);
    }
    return false;
}

/*
 * Real JS8 spot. Encodes an APSPOT-format @APRSIS CMD message via libx6100js8
 * and transmits it as JS8Call would: per-frame PTT cycles aligned to 15-sec
 * slot boundaries, with the radio temporarily switched to USB-DIG mode for
 * the duration so the digital-audio path is open.
 *
 * Wire format (confirmed by KI9NG live test 2026-05-09):
 *   @APRSIS CMD :APSPOT   :! POTA <REF> <FREQ_MHz> <MODE>
 *
 * The library returns one PCM frame per JS8 frame (4–5 for a typical spot),
 * each exactly X6100JS8_SAMPLES_PER_FRAME (606720) samples at 48 kHz. No
 * inter-frame silence — that's this scheduler's job. We resample each frame
 * individually to AUDIO_PLAY_RATE (44100) and play it inside a PTT cycle,
 * then drop PTT and wait for the next slot boundary.
 *
 * Per-frame PTT (instead of one big TX) so the operator's amp/relay only
 * runs during actual modulation. PTT drops in the 2.36 sec gap between
 * frames. Mirrors what dialog_ft8.c::tx_worker() does for FT8.
 *
 * Audio path: AUDIO_PLAY_RATE PCM goes to PulseAudio "AIF1 DA0" sink, which
 * the radio's audio mixer routes to the digital input ONLY when the VFO
 * mode is one of the *_dig modes. Hence the temporary usb_dig switch.
 */
static void js8_do_spot(const char *park, uint32_t dial_hz, bool tune_atu) {
    /* ─── preflight ───────────────────────────────────────────────────── */
    if (params.callsign.x[0] == '\0') {
        msg_schedule_text_fmt("Set callsign in Settings first");
        dialog_destruct();
        return;
    }

    const char *mode     = pota_spot_mode_str();
    double      freq_mhz = (double)dial_hz / 1000000.0;

    msg_schedule_text_fmt("JS8 encoding %s...", park);

    /* ─── encode: returns per-frame PCM @ 48 kHz, no inter-frame silence
     * (the engine refactor moved silence ownership to this scheduler). ── */
    x6100js8_msg_t *msg = NULL;
    int rc = x6100js8_encode_pota_spot(
                params.callsign.x, park, freq_mhz, mode,
                1500.0, &msg);
    if (rc != 0 || !msg) {
        msg_schedule_text_fmt("JS8 encode error %d", rc);
        dialog_destruct();
        return;
    }

    int n_frames = x6100js8_msg_frame_count(msg);
    if (n_frames < 1) {
        msg_schedule_text_fmt("JS8 encoded 0 frames");
        x6100js8_msg_free(msg);
        dialog_destruct();
        return;
    }

    /* ─── QSY ─────────────────────────────────────────────────────────── */
    int32_t saved_freq = subject_get_int(cfg_cur.fg_freq);
    int32_t saved_mode = subject_get_int(cfg_cur.mode);
    radio_set_freq(dial_hz);

    /* ─── ATU tune FIRST, in the user's current mode ─────────────────────
     * Has to happen before the usb_dig switch because radio.c's ATU state
     * machine calls recover_processing_audio_inputs() at completion, which
     * ends by restoring the radio's mode to subject_get_int(cfg_cur.mode).
     * If we switched mode before tuning, that restore would clobber it. */
    if (tune_atu) {
        msg_schedule_text_fmt("ATU tune...");
        radio_start_atu();
        if (!js8_wait_for_atu_done(35)) {
            msg_schedule_text_fmt("ATU tune timeout");
            radio_set_freq(saved_freq);
            x6100js8_msg_free(msg);
            dialog_destruct();
            return;
        }
    }

    /* ─── switch radio to USB-DIG so audio routing reaches the modulator ──
     * Use subject_set_int (NOT the bare x6100_control_vfo_mode_set) so the
     * subject observer chain actually sends the command to the radio AND
     * updates whatever bookkeeping depends on the cfg state. */
    subject_set_int(cfg_cur.mode, x6100_mode_usb_dig);
    /* Give the subject observer + radio command queue a moment to settle
     * before we start keying. */
    usleep(100000);

    /* Announce alignment wait so the operator knows the radio isn't dead. */
    {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        double sec_full = (double)now.tv_sec + (double)now.tv_nsec / 1.0e9;
        float  into_slot = (float)fmod(sec_full, 15.0);
        float  wait_s    = (into_slot < (float)X6100JS8_LATE_WINDOW_MS / 1000.0f)
                           ? 0.0f
                           : (15.0f - into_slot);
        msg_schedule_text_fmt("JS8 %s %d frames (wait %.0fs)",
                              park, n_frames, wait_s);
    }
    js8_wait_for_next_slot();

    /* ─── per-frame TX loop ───────────────────────────────────────────── */
    for (int i = 0; i < n_frames; i++) {
        size_t          frame48_n = 0;
        const int16_t  *frame48   = x6100js8_msg_frame(msg, i, &frame48_n);
        if (!frame48 || frame48_n == 0) {
            msg_schedule_text_fmt("JS8 frame %d missing", i + 1);
            break;
        }

        /* Resample THIS frame from 48 kHz to AUDIO_PLAY_RATE (44100).
         * Exact output size: frame48_n * 44100 / 48000 = 556920 samples
         * for the canonical 606720-sample frame. */
        size_t   play_n = 0;
        int16_t *play   = js8_resample_linear(frame48, frame48_n,
                                              X6100JS8_SAMPLE_RATE,
                                              AUDIO_PLAY_RATE, &play_n);
        if (!play) {
            msg_schedule_text_fmt("JS8 alloc failed");
            break;
        }

        msg_schedule_text_fmt("JS8 TX %d/%d", i + 1, n_frames);

        /* Key PTT, wait JS8A_START_DELAY_MS (500 ms) for PA/relay to settle,
         * then play. Receivers expect this silence before the Costas arrays. */
        radio_set_modem(true);
        usleep(X6100JS8_PTT_DELAY_MS * 1000);

        const int16_t *ptr    = play;
        size_t         remain = play_n;
        while (remain > 0) {
            size_t chunk = (remain < 2048) ? remain : 2048;
            audio_play((int16_t *)ptr, chunk);
            ptr    += chunk;
            remain -= chunk;
        }
        audio_play_wait();
        radio_set_modem(false);
        free(play);

        /* If there's another frame coming, wait for the next slot boundary.
         * The 2.16 s late-start window in js8_wait_for_next_slot absorbs any
         * PulseAudio buffer-drain lag (see wiki Bugs 3+4 audit). */
        if (i + 1 < n_frames) {
            js8_wait_for_next_slot();
        }
    }

    /* ─── restore radio state ─────────────────────────────────────────── */
    subject_set_int(cfg_cur.mode, saved_mode);
    radio_set_freq(saved_freq);
    x6100js8_msg_free(msg);

    msg_schedule_text_fmt("Spotted %s on JS8", park);
    dialog_destruct();
}

/* ─── list view ─────────────────────────────────────────────────────────── */

static void list_btn_click_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= park_refs_n) return;

    /* Add nearby selections to recent history before showing method picker.
     * pota_parks_add is a no-op if the park is already at the top of recent. */
    pota_parks_add(park_refs[idx]);

    strncpy(selected_park, park_refs[idx], sizeof(selected_park) - 1);
    selected_park[sizeof(selected_park) - 1] = '\0';

    show_method(selected_park);
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

/* Format a park row label: "REF        4.2 km  Park Name" / "? km" / "(unknown)". */
static void format_row(char *buf, size_t buflen, const char *ref,
                       bool have_fix, double lat, double lon)
{
    const pota_db_entry_t *e = pota_db_lookup(ref);
    if (!e) {
        snprintf(buf, buflen, "%-10s  (unknown)", ref);
        return;
    }
    if (!have_fix) {
        snprintf(buf, buflen, "%-10s  ? km    %s", ref, e->name);
        return;
    }
    float d = pota_db_dist_km(lat, lon, e);
    if (d < 10.0f)
        snprintf(buf, buflen, "%-10s  %4.1f km  %s", ref, d, e->name);
    else if (d < 1000.0f)
        snprintf(buf, buflen, "%-10s  %4.0f km  %s", ref, d, e->name);
    else
        snprintf(buf, buflen, "%-10s  >999 km  %s", ref, e->name);
}

static void show_list(void) {
    if (!body) return;
    view = VIEW_LIST;

    lv_obj_clean(body);
    park_refs_n = 0;

    buttons_load_page(&page_list);

    lv_obj_t *list = lv_list_create(body);
    lv_obj_set_size(list, LIST_W, BODY_H);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_bg_opa(list, LV_OPA_20, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);

    lv_obj_t *first_btn = NULL;

    /* Load DB unconditionally — RECENT rows want names too. */
    bool have_db  = pota_db_load() && pota_db_ready();
    double lat = 0.0, lon = 0.0;
    bool have_fix = gps_get_fix(&lat, &lon);

    /* RECENT */
    int recent_n = pota_parks_count();
    if (recent_n > 0) {
        add_section_header(list, "── RECENT ──");
        for (int i = 0; i < recent_n && park_refs_n < MAX_REFS; i++) {
            const char *park = pota_parks_get(i);
            if (!park) continue;

            strncpy(park_refs[park_refs_n], park, POTA_DB_REF_LEN - 1);
            park_refs[park_refs_n][POTA_DB_REF_LEN - 1] = '\0';

            char label[80];
            if (have_db)
                format_row(label, sizeof(label), park, have_fix, lat, lon);
            else
                snprintf(label, sizeof(label), "%s", park);

            lv_obj_t *btn = add_park_row(list, label, park_refs_n);
            if (!first_btn) first_btn = btn;

            park_refs_n++;
        }
    }

    /* NEARBY */
    if (have_fix && have_db) {
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

    /* Empty state */
    if (park_refs_n == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "Tap New Park to enter a reference");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x808080), 0);
        lv_obj_set_style_text_font(empty, &sony_22, 0);
        lv_obj_center(empty);
    }

    if (first_btn)
        lv_group_focus_obj(first_btn);

    if (title_lbl) {
        if (park_refs_n > 0)
            lv_label_set_text(title_lbl, "MFK: scroll   Press: pick park");
        else
            lv_label_set_text(title_lbl, "No parks — tap New Park");
    }
}

/* ─── method view ───────────────────────────────────────────────────────── */

static lv_obj_t *make_action_btn(lv_obj_t *parent, const char *label,
                                 lv_event_cb_t cb, int x_off, int y_off,
                                 int w, int h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x_off, y_off);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A3A5C), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E6FBF), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(keyboard_group, btn);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &sony_22, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    return btn;
}

static void method_wifi_btn_cb(lv_event_t *e) {
    (void)e;
    wifi_do_spot(selected_park);
}

static void method_js8_btn_cb(lv_event_t *e) {
    (void)e;
    show_band();
}

static void show_method(const char *park) {
    if (!body) return;
    view = VIEW_METHOD;

    lv_obj_clean(body);
    buttons_load_page(&page_method);

    /* Banner */
    lv_obj_t *banner = lv_label_create(body);
    lv_label_set_text_fmt(banner, "Spot %s via...", park);
    lv_obj_set_style_text_font(banner, &sony_22, 0);
    lv_obj_set_style_text_color(banner, lv_color_white(), 0);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 16);

    /* Show WiFi status under the banner so the operator knows whether the
     * WiFi path is even available before tapping it. */
    lv_obj_t *wifi_state = lv_label_create(body);
    bool connected = (wifi_get_status() == WIFI_CONNECTED);
    if (connected)
        lv_label_set_text(wifi_state, "WiFi: connected");
    else
        lv_label_set_text(wifi_state, "WiFi: not connected");
    lv_obj_set_style_text_font(wifi_state, &sony_22, 0);
    lv_obj_set_style_text_color(wifi_state,
        connected ? lv_color_hex(0x80C080) : lv_color_hex(0xC08080), 0);
    lv_obj_align(wifi_state, LV_ALIGN_TOP_MID, 0, 56);

    const int btn_w  = 280;
    const int btn_h  = 64;
    const int gap    = 24;
    const int row_y  = 120;

    lv_obj_t *wifi_btn = make_action_btn(body, "Send via WiFi",
        method_wifi_btn_cb, -(btn_w / 2 + gap / 2), row_y, btn_w, btn_h);
    lv_obj_t *js8_btn  = make_action_btn(body, "Send via JS8Call",
        method_js8_btn_cb, +(btn_w / 2 + gap / 2), row_y, btn_w, btn_h);

    /* Dim WiFi button if there's no link, but leave it clickable so the user
     * gets the same "No WiFi — spot failed" message they would have gotten
     * from a direct spot attempt. (Some environments show CONNECTED late.) */
    if (!connected) {
        lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x404040), 0);
    }

    lv_group_focus_obj(connected ? wifi_btn : js8_btn);

    if (title_lbl)
        lv_label_set_text(title_lbl, "Pick a method");
}

/* ─── band view ─────────────────────────────────────────────────────────── */

static void band_btn_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= JS8_BANDS_N) return;

    /* Repaint the previously-selected button back to default and the new
     * one to the selected color. */
    for (int i = 0; i < JS8_BANDS_N; i++) {
        if (!band_btns[i]) continue;
        lv_obj_set_style_bg_color(band_btns[i],
            (i == idx) ? lv_color_hex(0xC08020)   /* selected = amber */
                       : lv_color_hex(0x1A3A5C),  /* default */
            0);
    }
    js8_band_idx = idx;

    if (title_lbl)
        lv_label_set_text_fmt(title_lbl, "JS8 on %s @ %u kHz",
            js8_bands[idx].label, js8_bands[idx].dial_hz / 1000);
}

static const char *atu_label_getter(void) {
    static char buf[32];
    sprintf(buf, "ATU Tune:\n%s", js8_tune_atu ? "On" : "Off");
    return buf;
}

static void atu_toggle_cb(struct button_item_t *btn) {
    js8_tune_atu = !js8_tune_atu;
    /* BTN_TEXT_FN with no .subj doesn't auto-redraw on state change.
     * Force the footer button to re-call label_fn() so the text updates. */
    if (btn) buttons_refresh(btn);
}

static void send_js8_btn_cb(lv_event_t *e) {
    (void)e;
    if (js8_band_idx < 0) {
        msg_schedule_text_fmt("Pick a band first");
        return;
    }
    js8_do_spot(selected_park, js8_bands[js8_band_idx].dial_hz, js8_tune_atu);
}

static void show_band(void) {
    if (!body) return;
    view = VIEW_BAND;
    js8_band_idx = -1;
    js8_tune_atu = false;
    memset(band_btns, 0, sizeof(band_btns));

    lv_obj_clean(body);
    buttons_load_page(&page_band);

    /* Banner */
    lv_obj_t *banner = lv_label_create(body);
    lv_label_set_text_fmt(banner, "JS8 spot %s — pick band", selected_park);
    lv_obj_set_style_text_font(banner, &sony_22, 0);
    lv_obj_set_style_text_color(banner, lv_color_white(), 0);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 8);

    /* 4×2 grid of band buttons */
    const int btn_w = 140;
    const int btn_h = 56;
    const int x_gap = 12;
    const int y_gap = 12;
    const int grid_cols = 4;
    const int grid_y = 50;
    const int grid_total_w = grid_cols * btn_w + (grid_cols - 1) * x_gap;
    const int x0 = -(grid_total_w / 2) + (btn_w / 2);

    lv_obj_t *first_band_btn = NULL;
    for (int i = 0; i < JS8_BANDS_N; i++) {
        int col = i % grid_cols;
        int row = i / grid_cols;
        int x = x0 + col * (btn_w + x_gap);
        int y = grid_y + row * (btn_h + y_gap);

        lv_obj_t *btn = lv_btn_create(body);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, x, y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A3A5C), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E6FBF), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, band_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(keyboard_group, btn);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "%s\n%u", js8_bands[i].label,
                              js8_bands[i].dial_hz / 1000);
        lv_obj_set_style_text_font(lbl, &sony_22, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);

        band_btns[i] = btn;
        if (!first_band_btn) first_band_btn = btn;
    }

    /* ATU tune is now an F-key footer button (btn_atu); see page_band.
     * No body widget needed, which avoids the encoder edit-mode trap that
     * the old LVGL checkbox had — F-keys are physical buttons not encoder. */

    /* Send button */
    int send_y = grid_y + 2 * (btn_h + y_gap) + 32;
    lv_obj_t *send_btn = make_action_btn(body, "Send",
        send_js8_btn_cb, 0, send_y, 200, 48);
    (void)send_btn;

    if (first_band_btn)
        lv_group_focus_obj(first_band_btn);

    if (title_lbl)
        lv_label_set_text(title_lbl, "Pick a band, optional ATU");
}

/* ─── footer-button callbacks ───────────────────────────────────────────── */

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
    show_list();
    msg_schedule_text_fmt("Refreshed");
}

static void btn_cancel_cb(struct button_item_t *btn) {
    (void)btn;
    dialog_destruct();
}

static void btn_back_cb(struct button_item_t *btn) {
    (void)btn;
    /* METHOD → LIST, BAND → METHOD */
    if (view == VIEW_BAND)
        show_method(selected_park);
    else
        show_list();
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

    /* Manually-entered parks go through the method picker too — same flow as
     * picking from the list. The dialog body needs to come back first. */
    if (dialog.obj)
        lv_obj_clear_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);

    strncpy(selected_park, park, sizeof(selected_park) - 1);
    selected_park[sizeof(selected_park) - 1] = '\0';
    show_method(selected_park);
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
    selected_park[0] = '\0';
    js8_band_idx = -1;
    js8_tune_atu = false;

    title_lbl = lv_label_create(dialog.obj);
    lv_label_set_text(title_lbl, "Loading...");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(title_lbl, &sony_22, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 6);

    /* Body container: a transparent obj whose children we swap on each view
     * transition. Owning a body separate from dialog.obj means the title
     * label survives lv_obj_clean(body) calls — view transitions don't
     * have to rebuild the title each time. */
    body = lv_obj_create(dialog.obj);
    lv_obj_set_size(body, LIST_W, BODY_H);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, TITLE_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    show_list();
}

static void destruct_cb(void) {
    if (in_textarea) {
        textarea_window_close();
        in_textarea = false;
    }
    body         = NULL;
    title_lbl    = NULL;
    park_refs_n  = 0;
    js8_band_idx = -1;
    js8_tune_atu = false;
    selected_park[0] = '\0';
    memset(band_btns, 0, sizeof(band_btns));
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    switch (key) {
        case LV_KEY_ESC:
            if (in_textarea) {
                textarea_window_close();
                in_textarea = false;
                /* If we were on the list view when New Park was tapped,
                 * the dialog was hidden; show it again. */
                if (dialog.obj)
                    lv_obj_clear_flag(dialog.obj, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            /* Drill back through the views: BAND → METHOD → LIST → exit. */
            if (view == VIEW_BAND)
                show_method(selected_park);
            else if (view == VIEW_METHOD)
                show_list();
            else
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
