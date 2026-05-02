/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot — WiFi path via POTA API
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Call once after database_init() and before any pota_spot_wifi() calls.
 * Runs curl_global_init(CURL_GLOBAL_DEFAULT).
 */
void pota_spot_init(void);

/**
 * Call on app shutdown to release curl resources.
 */
void pota_spot_cleanup(void);

/**
 * Post a self-spot to https://api.pota.app/spot/
 *
 * Callsign is read from params.callsign.x (the same one used by FT8).
 * On success the park is saved to the local history via pota_parks_add().
 *
 * Blocks until the HTTP request completes (typically < 1 s on good WiFi).
 * Returns true on HTTP 200, false on any error.
 *
 * @param park     Park reference e.g. "US-0765"
 * @param freq_hz  Operating frequency in Hz
 * @param mode     Mode string: "SSB", "CW", "FT8", "AM", "FM"
 * @param comment  Optional comment (NULL → default string)
 */
bool pota_spot_wifi(const char *park, int32_t freq_hz,
                    const char *mode, const char *comment);
