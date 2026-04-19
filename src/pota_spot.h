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
 * Post a self-spot to https://api.pota.app/spot/
 * Blocks until the HTTP request completes (typically < 1 second on good WiFi).
 * Returns true on HTTP 200, false on any error.
 *
 * @param park     Park reference, e.g. "K-1234"
 * @param freq_hz  Operating frequency in Hz
 * @param mode     Mode string: "SSB", "CW", "FT8", "AM", "FM"
 * @param comment  Optional comment (NULL for default)
 */
bool pota_spot_wifi(const char *park, int32_t freq_hz,
                    const char *mode, const char *comment);
