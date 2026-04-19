/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot — core logic
 *
 *  Phase 1: WiFi → https://api.pota.app/spot/  (immediate, no RF)
 *  Phase 2: FT8 free-text via SOTAmat suffix    (TODO: needs blob parser)
 *
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#include "pota_spot.h"

#include "audio.h"
#include "cfg/cfg.h"
#include "cfg/digital_modes.h"
#include "ft8/worker.h"
#include "lvgl/lvgl.h"
#include "params/params.h"
#include "radio.h"
#include "scheduler.h"
#include "wifi.h"

#include <curl/curl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ─── constants ─────────────────────────────────────────────────────────── */

#define POTA_API_URL    "https://api.pota.app/spot/"
#define POTA_API_ORIGIN "https://pota.app"
#define POTA_SOURCE     "X6100-firmware"

#define FT8_SIGNAL_FREQ 1325        /* Hz — audio offset used by dialog_ft8 */
#define FT8_TX_REPEATS  4           /* transmit 4 slots for skimmer coverage */
#define FT8_SLOT_SEC    15.0f       /* FT8 slot length in seconds */
#define FT8_MAX_START_DELAY 1.5f    /* don't TX in last 1.5 s of a slot */
#define HTTP_TIMEOUT_SEC 10L

/* ─── state ──────────────────────────────────────────────────────────────── */

static pthread_t              spot_thread;
static atomic_bool            cancel_flag;
static atomic_bool            busy_flag;
static pota_spot_status_cb_t  user_cb;
static char                   last_park[16];    /* survives calls */
static char                   current_park[16]; /* active spot */

/* ─── scheduler shim ─────────────────────────────────────────────────────── */

/* scheduler_put() dispatches a memcpy'd payload to the LVGL thread.
   We box the (state, tx_num) pair into a small struct. */
typedef struct {
    pota_spot_status_cb_t cb;
    pota_spot_state_t     state;
    int                   tx_num;
} cb_payload_t;

static void dispatch_cb_lvgl(void *data) {
    cb_payload_t *p = (cb_payload_t *)data;
    if (p->cb) p->cb(p->state, p->tx_num);
}

static void fire_cb(pota_spot_state_t state, int tx_num) {
    if (!user_cb) return;
    cb_payload_t p = { .cb = user_cb, .state = state, .tx_num = tx_num };
    scheduler_put(dispatch_cb_lvgl, &p, sizeof(p));
}

/* ─── mode → string ──────────────────────────────────────────────────────── */

static const char *mode_str(void) {
    /* cfg_cur.mode holds x6100_mode_t */
    int m = subject_get_int(cfg_cur.mode);
    switch ((x6100_mode_t)m) {
        case x6100_mode_lsb:  return "SSB";
        case x6100_mode_usb:  return "SSB";
        case x6100_mode_cw:   return "CW";
        case x6100_mode_cwr:  return "CW";
        case x6100_mode_am:   return "AM";
        case x6100_mode_nfm:  return "FM";
        default:              return "SSB";
    }
}

/* ─── libcurl helpers ────────────────────────────────────────────────────── */

/* Discard response body — we only care about HTTP status code */
static size_t curl_discard(void *ptr, size_t size, size_t nmemb, void *ud) {
    (void)ptr; (void)ud;
    return size * nmemb;
}

/* ─── Phase 1: WiFi → POTA API ───────────────────────────────────────────── */

static bool try_wifi_spot(const char *park, int32_t freq_hz, const char *mode) {
    if (wifi_get_status() != WIFI_CONNECTED) return false;

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    /* Build JSON body */
    char body[512];
    snprintf(body, sizeof(body),
        "{\"activator\":\"%s\","
        "\"spotter\":\"%s\","
        "\"frequency\":\"%.1f\","
        "\"reference\":\"%s\","
        "\"mode\":\"%s\","
        "\"source\":\"%s\","
        "\"comments\":\"Self-spotted from X6100 firmware\"}",
        params.callsign.x,
        params.callsign.x,
        freq_hz / 1000.0,   /* Hz → kHz */
        park,
        mode,
        POTA_SOURCE);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Origin: " POTA_API_ORIGIN);
    hdrs = curl_slist_append(hdrs, "Referer: " POTA_API_ORIGIN "/");
    hdrs = curl_slist_append(hdrs, "User-Agent: X6100-firmware/1.0");

    curl_easy_setopt(curl, CURLOPT_URL,            POTA_API_URL);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        HTTP_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_discard);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    bool ok = (res == CURLE_OK && code == 200);
    if (!ok) {
        LV_LOG_WARN("POTA API: curl=%d http=%ld", (int)res, code);
    }
    return ok;
}

/* ─── Phase 2: FT8 free-text (SOTAmat) ──────────────────────────────────── */
/*
 * TODO Phase 2 — SOTAmat suffix lookup from config blob on SD card.
 *
 * The SOTAmat system uses a personal server-generated lookup table that maps
 * (park_ref × freq_bucket × time_slot) → 4-char suffix.  The config blob
 * format is not publicly documented; contact AB6D (sotamat@sotamat.com) or
 * reverse-engineer the mobile app to determine format.
 *
 * Once the suffix is known, the FT8 message is:
 *   "S KI9NG/XXXX"   (13 chars, free-text FT8 type 0.3)
 *
 * Message is transmitted 4× on the nearest FT8 band frequency, one per
 * 15-second slot, aligned to UTC slot boundaries.  Only SparkSDR (~94
 * stations) and CWSL_DIGI v0.88+ (~30 stations) decode free-text FT8
 * callsigns and forward them to PSKreporter for SOTAmat pickup.
 *
 * Stub below returns false (not implemented) until blob parsing is done.
 */

static bool sotamat_suffix_lookup(const char *park, char *suffix_out, size_t sz) {
    (void)park;
    /* Phase 2: parse /mnt/DATA/sotamat.blob, look up suffix for park */
    (void)suffix_out;
    (void)sz;
    return false; /* not yet implemented */
}

static bool try_ft8_spot(const char *park) {
    char suffix[8] = {0};
    if (!sotamat_suffix_lookup(park, suffix, sizeof(suffix))) {
        LV_LOG_INFO("POTA: no SOTAmat suffix — FT8 path unavailable");
        return false;
    }

    /* Build 13-char FT8 free-text message: "S CALL/XXXX" */
    char msg[14];
    int len = snprintf(msg, sizeof(msg), "S %s/%s",
                       params.callsign.x, suffix);
    if (len > 13) {
        LV_LOG_WARN("POTA FT8 message too long (%d chars): %s", len, msg);
        return false;
    }

    /* Save radio state */
    int32_t saved_freq = subject_get_int(cfg_cur.fg_freq);

    /* QSY to nearest FT8 band frequency */
    if (!cfg_digital_load(0, CFG_DIG_TYPE_FT8)) {
        LV_LOG_WARN("POTA: no FT8 band frequency found");
        return false;
    }
    int32_t ft8_dial = subject_get_int(cfg_cur.fg_freq);

    /* Encode FT8 message → PCM */
    ftx_worker_init(AUDIO_PLAY_RATE, FTX_PROTOCOL_FT8);
    int16_t  *samples   = NULL;
    uint32_t  n_samples = 0;
    if (!ftx_worker_generate_tx_samples(msg, FT8_SIGNAL_FREQ, AUDIO_PLAY_RATE,
                                        &samples, &n_samples)) {
        LV_LOG_ERROR("POTA: FT8 encode failed for '%s'", msg);
        radio_set_freq(saved_freq);
        subject_set_int(cfg_cur.fg_freq, saved_freq);
        return false;
    }

    /* Set audio gain (same as dialog_ft8 base gain for patched firmware) */
    audio_set_play_vol(-9.4f + 6.0f);

    bool success = true;

    for (int i = 0; i < FT8_TX_REPEATS && !atomic_load(&cancel_flag); i++) {
        /* Wait for next 15-second UTC slot boundary */
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        float elapsed_in_slot = fmodf(
            (float)now.tv_sec + now.tv_nsec / 1e9f,
            FT8_SLOT_SEC);
        float wait = FT8_SLOT_SEC - elapsed_in_slot;
        if (wait < FT8_MAX_START_DELAY) wait += FT8_SLOT_SEC;

        /* Sleep in short increments so cancel is responsive */
        uint64_t wait_us = (uint64_t)(wait * 1e6f);
        while (wait_us > 0 && !atomic_load(&cancel_flag)) {
            uint64_t chunk = wait_us > 100000 ? 100000 : wait_us;
            usleep((useconds_t)chunk);
            wait_us -= chunk;
        }
        if (atomic_load(&cancel_flag)) { success = false; break; }

        fire_cb(POTA_SPOT_FT8_TRANSMITTING, i + 1);

        /* Tune radio: dial + audio_offset - signal_freq aligns tone on air */
        radio_set_freq(ft8_dial + params.ft8_tx_freq.x - FT8_SIGNAL_FREQ);
        radio_set_modem(true);

        int16_t  *ptr  = samples;
        uint32_t  left = n_samples;
        while (left > 0 && !atomic_load(&cancel_flag)) {
            size_t chunk = left > 2048 ? 2048 : left;
            audio_play(ptr, chunk);
            left -= chunk;
            ptr  += chunk;
        }

        audio_play_wait();
        radio_set_modem(false);
        radio_set_freq(ft8_dial);   /* back to dial between slots */
    }

    free(samples);

    /* Restore original freq and update UI subject */
    radio_set_freq(saved_freq);
    subject_set_int(cfg_cur.fg_freq, saved_freq);
    audio_set_play_vol(params.play_gain_db_f.x);

    return success && !atomic_load(&cancel_flag);
}

/* ─── worker thread ──────────────────────────────────────────────────────── */

static void *spot_worker(void *arg) {
    (void)arg;

    int32_t freq_hz = subject_get_int(cfg_cur.fg_freq);
    const char *mode = mode_str();

    /* ── Phase 1: WiFi ── */
    fire_cb(POTA_SPOT_WIFI_POSTING, 0);
    if (try_wifi_spot(current_park, freq_hz, mode)) {
        fire_cb(POTA_SPOT_DONE, 0);
        atomic_store(&busy_flag, false);
        return NULL;
    }

    if (atomic_load(&cancel_flag)) {
        fire_cb(POTA_SPOT_CANCELLED, 0);
        atomic_store(&busy_flag, false);
        return NULL;
    }

    /* ── Phase 2: FT8 via SOTAmat (stub) ── */
    fire_cb(POTA_SPOT_FT8_WAITING, 0);
    if (try_ft8_spot(current_park)) {
        fire_cb(POTA_SPOT_DONE, 0);
        atomic_store(&busy_flag, false);
        return NULL;
    }

    if (atomic_load(&cancel_flag)) {
        fire_cb(POTA_SPOT_CANCELLED, 0);
    } else {
        /* No WiFi and no SOTAmat config → tell the user */
        fire_cb(POTA_SPOT_FAILED_NO_WIFI, 0);
    }

    atomic_store(&busy_flag, false);
    return NULL;
}

/* ─── public API ─────────────────────────────────────────────────────────── */

void pota_spot_start(const char *park_ref, pota_spot_status_cb_t cb) {
    if (atomic_load(&busy_flag)) {
        LV_LOG_WARN("POTA spot already in progress");
        return;
    }

    strncpy(current_park, park_ref, sizeof(current_park) - 1);
    current_park[sizeof(current_park) - 1] = '\0';
    strncpy(last_park, current_park, sizeof(last_park));

    user_cb = cb;
    atomic_store(&cancel_flag, false);
    atomic_store(&busy_flag, true);

    pthread_create(&spot_thread, NULL, spot_worker, NULL);
    pthread_detach(spot_thread);
}

void pota_spot_cancel(void) {
    atomic_store(&cancel_flag, true);
}

bool pota_spot_busy(void) {
    return atomic_load(&busy_flag);
}

const char *pota_spot_last_park(void) {
    return last_park;
}
