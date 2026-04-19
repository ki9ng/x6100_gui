/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot feature
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    POTA_SPOT_IDLE = 0,
    POTA_SPOT_WIFI_POSTING,       /* attempting WiFi → POTA API */
    POTA_SPOT_FT8_WAITING,        /* waiting for FT8 slot boundary */
    POTA_SPOT_FT8_TRANSMITTING,   /* actively TXing an FT8 slot */
    POTA_SPOT_DONE,
    POTA_SPOT_FAILED_NO_WIFI,
    POTA_SPOT_FAILED_API,
    POTA_SPOT_FAILED_FT8,
    POTA_SPOT_CANCELLED,
} pota_spot_state_t;

/**
 * Status callback — fired from worker thread via scheduler_put().
 * @param state   current state
 * @param tx_num  FT8 TX slot (1–4), 0 when not in FT8 path
 */
typedef void (*pota_spot_status_cb_t)(pota_spot_state_t state, int tx_num);

/**
 * Start a POTA self-spot in a background pthread.
 *
 * WiFi path attempted first.  Falls back to FT8 (SOTAmat free-text) if
 * a SOTAmat suffix is configured (Phase 2).  Calls cb() on every state
 * change; cb() is dispatched to the LVGL thread via scheduler_put().
 *
 * @param park_ref   e.g. "K-1234"
 * @param cb         status callback (may be NULL)
 */
void pota_spot_start(const char *park_ref, pota_spot_status_cb_t cb);

/**
 * Request cancellation of an in-progress spot.
 * Returns immediately; worker stops at the next safe point.
 */
void pota_spot_cancel(void);

/** True while a spot operation is running */
bool pota_spot_busy(void);

/** Last park reference used (persists across calls, pre-fills the dialog) */
const char *pota_spot_last_park(void);
