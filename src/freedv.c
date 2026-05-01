/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI — FreeDV 700D audio bridge (ki9ng fork)
 *
 *  TX path: mic PA source → codec2 encode → SSB play sink (USB-DIG mode)
 *  RX path: SSB capture source → codec2 decode → speaker play sink
 *
 *  PulseAudio is asked for 8 kHz mono; it handles 44.1 kHz ↔ 8 kHz resampling.
 *  Each path runs in a dedicated pthread to keep the LVGL UI thread free.
 *
 *  Thread stop protocol: set {tx,rx}_run = false; the thread finishes its
 *  current pa_simple_read() (≤56 ms at 8 kHz for 700D) and exits cleanly.
 *  pthread_join() then collects it (max ~100 ms UI freeze on PTT transition).
 */

#include <stdint.h>   /* must precede audio.h which uses int16_t */
#include "freedv.h"
#include "audio.h"
#include "params/params.h"

/* Include codec2 AFTER our own headers so our fdv_get_mode / fdv_set_mode
 * declarations are already in scope before the codec2 freedv_get_mode(struct freedv *)
 * declaration appears. */
#include <codec2/freedv_api.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* Map our app enum to codec2 integer mode constants */
static int app_to_codec2_mode(freedv_mode_t m) {
    switch (m) {
        case FDV_MODE_700D: return FREEDV_MODE_700D;   /* 7 in freedv_api.h */
        case FDV_MODE_1600: return FREEDV_MODE_1600;   /* 0 in freedv_api.h */
        default:            return -1;
    }
}

static struct freedv    *fdv         = NULL;
static pthread_t         tx_thread;
static pthread_t         rx_thread;
static volatile bool     tx_run      = false;
static volatile bool     rx_run      = false;
static bool              initialised  = false;

/* ── PulseAudio helpers ──────────────────────────────────────────────────── */

static pa_simple *pa_open_simple(pa_stream_direction_t dir, const char *name) {
    static const pa_sample_spec spec = {
        .format   = PA_SAMPLE_S16LE,
        .rate     = 8000,
        .channels = 1,
    };
    int err;
    pa_simple *s = pa_simple_new(NULL, "X6100 FreeDV", dir,
                                 NULL, name, &spec, NULL, NULL, &err);
    if (!s)
        fprintf(stderr, "[freedv] pa_simple_new(%s) failed: %s\n",
                name, pa_strerror(err));
    return s;
}

/* ── TX worker ───────────────────────────────────────────────────────────── */

static void *tx_worker(void *arg) {
    (void)arg;
    if (!fdv) return NULL;

    int n_speech = freedv_get_n_speech_samples(fdv);
    int n_modem  = freedv_get_n_nom_modem_samples(fdv);

    int16_t *speech = malloc(n_speech * sizeof(int16_t));
    int16_t *modem  = malloc(n_modem  * sizeof(int16_t));
    pa_simple *rec  = pa_open_simple(PA_STREAM_RECORD,   "FreeDV Mic");
    pa_simple *play = pa_open_simple(PA_STREAM_PLAYBACK, "FreeDV TX");

    if (!speech || !modem || !rec || !play) {
        fprintf(stderr, "[freedv] tx_worker: resource alloc failed\n");
        goto cleanup;
    }

    while (tx_run) {
        int err;
        if (pa_simple_read(rec, speech, n_speech * sizeof(int16_t), &err) < 0) {
            if (tx_run)
                fprintf(stderr, "[freedv] tx read: %s\n", pa_strerror(err));
            break;
        }
        freedv_tx(fdv, modem, speech);
        if (pa_simple_write(play, modem, n_modem * sizeof(int16_t), &err) < 0) {
            if (tx_run)
                fprintf(stderr, "[freedv] tx write: %s\n", pa_strerror(err));
            break;
        }
    }

cleanup:
    free(speech);
    free(modem);
    if (rec)  pa_simple_free(rec);
    if (play) { pa_simple_drain(play, NULL); pa_simple_free(play); }
    return NULL;
}

/* ── RX worker ───────────────────────────────────────────────────────────── */

static void *rx_worker(void *arg) {
    (void)arg;
    if (!fdv) return NULL;

    int n_speech = freedv_get_n_speech_samples(fdv);
    int n_modem  = freedv_get_n_nom_modem_samples(fdv);

    int16_t *modem  = malloc(n_modem  * sizeof(int16_t));
    int16_t *speech = malloc(n_speech * sizeof(int16_t));
    pa_simple *rec  = pa_open_simple(PA_STREAM_RECORD,   "FreeDV RX");
    pa_simple *play = pa_open_simple(PA_STREAM_PLAYBACK, "FreeDV Decoded");

    if (!modem || !speech || !rec || !play) {
        fprintf(stderr, "[freedv] rx_worker: resource alloc failed\n");
        goto cleanup;
    }

    while (rx_run) {
        int err;
        if (pa_simple_read(rec, modem, n_modem * sizeof(int16_t), &err) < 0) {
            if (rx_run)
                fprintf(stderr, "[freedv] rx read: %s\n", pa_strerror(err));
            break;
        }
        freedv_rx(fdv, speech, modem);
        if (pa_simple_write(play, speech, n_speech * sizeof(int16_t), &err) < 0) {
            if (rx_run)
                fprintf(stderr, "[freedv] rx write: %s\n", pa_strerror(err));
            break;
        }
    }

cleanup:
    free(modem);
    free(speech);
    if (rec)  pa_simple_free(rec);
    if (play) { pa_simple_drain(play, NULL); pa_simple_free(play); }
    return NULL;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Cycling callsign buffer for FreeDV text channel TX callback */
static char  txt_tx_buf[16];
static int   txt_tx_idx;

static char txt_tx_cb(void *state) {
    char c = txt_tx_buf[txt_tx_idx];
    if (++txt_tx_idx >= (int)strlen(txt_tx_buf))
        txt_tx_idx = 0;
    return c;
}

static void open_fdv(freedv_mode_t mode) {
    int cm = app_to_codec2_mode(mode);
    if (cm < 0) return;
    fdv = freedv_open(cm);
    if (!fdv) {
        fprintf(stderr, "[freedv] freedv_open(%d) failed\n", cm);
        return;
    }
    /* Register callsign as the cycled text-channel TX string */
    snprintf(txt_tx_buf, sizeof(txt_tx_buf), "%-6s ",
             params.callsign.x[0] ? params.callsign.x : "KI9NG");
    txt_tx_idx = 0;
    freedv_set_callback_txt(fdv, NULL, txt_tx_cb, NULL);
}

static void rx_start(void) {
    if (!fdv || rx_run) return;
    audio_play_en(false);
    rx_run = true;
    pthread_create(&rx_thread, NULL, rx_worker, NULL);
    fprintf(stderr, "[freedv] RX start\n");
}

static void rx_stop(void) {
    if (!rx_run) return;
    rx_run = false;
    pthread_join(rx_thread, NULL);
    audio_play_en(true);
    fprintf(stderr, "[freedv] RX stop\n");
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void freedv_init(void) {
    if (initialised) return;
    initialised = true;
    freedv_mode_t mode = fdv_get_mode();
    fprintf(stderr, "[freedv] init, mode=%s\n", freedv_mode_label(mode));
    if (mode != FDV_MODE_OFF) {
        open_fdv(mode);
        rx_start();
    }
}

void freedv_deinit(void) {
    if (!initialised) return;
    freedv_on_tx_stop();
    rx_stop();
    if (fdv) { freedv_close(fdv); fdv = NULL; }
    initialised = false;
}

freedv_mode_t fdv_get_mode(void) {
    return (freedv_mode_t)params.freedv_mode.x;
}

void fdv_set_mode(freedv_mode_t mode) {
    freedv_on_tx_stop();
    rx_stop();
    if (fdv) { freedv_close(fdv); fdv = NULL; }

    params_uint8_set(&params.freedv_mode, (uint8_t)mode);
    fprintf(stderr, "[freedv] mode -> %s\n", freedv_mode_label(mode));

    if (mode != FDV_MODE_OFF) {
        open_fdv(mode);
        rx_start();
    }
}

const char *freedv_mode_label(freedv_mode_t mode) {
    switch (mode) {
        case FDV_MODE_OFF:  return "OFF";
        case FDV_MODE_700D: return "700D";
        case FDV_MODE_1600: return "1600";
        default:            return "?";
    }
}

bool freedv_is_active(void) {
    return initialised && fdv_get_mode() != FDV_MODE_OFF;
}

void freedv_on_tx_start(void) {
    if (!freedv_is_active() || tx_run) return;
    rx_stop();
    tx_run = true;
    pthread_create(&tx_thread, NULL, tx_worker, NULL);
    fprintf(stderr, "[freedv] TX start\n");
}

void freedv_on_tx_stop(void) {
    if (!tx_run) return;
    tx_run = false;
    pthread_join(tx_thread, NULL);
    if (freedv_is_active())
        rx_start();
    fprintf(stderr, "[freedv] TX stop\n");
}

void freedv_get_stats(int *sync_out, float *snr_out) {
    if (!fdv || !initialised) {
        if (sync_out) *sync_out = 0;
        if (snr_out)  *snr_out  = 0.0f;
        return;
    }
    freedv_get_modem_stats(fdv, sync_out, snr_out);
}

void freedv_resync(void) {
    if (fdv) freedv_set_sync(fdv, 0);
}
