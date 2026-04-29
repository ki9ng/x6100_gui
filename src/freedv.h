/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI — FreeDV PoC scaffold (ki9ng fork)
 *
 *  Phase 0 (v0.1): module scaffold only. No codec2 calls; no audio bridge.
 *  See ROADMAP.md "FreeDV" section for the multi-phase plan.
 */

#pragma once

#include <stdbool.h>

typedef enum {
    FREEDV_MODE_OFF  = 0,
    FREEDV_MODE_700D = 1,
    FREEDV_MODE_1600 = 2,
} freedv_mode_t;

/// @brief Initialize the FreeDV module. Safe to call once at startup.
///        In v0.1 this only logs a banner — codec2 is not yet linked.
void freedv_init(void);

/// @brief Tear down the FreeDV module. Safe to call multiple times.
void freedv_deinit(void);

/// @brief Get the current FreeDV mode.
freedv_mode_t freedv_get_mode(void);

/// @brief Set the FreeDV mode. v0.1 stores the value but takes no further action.
void freedv_set_mode(freedv_mode_t mode);

/// @brief Convenience: human-readable label for a mode value.
const char *freedv_mode_label(freedv_mode_t mode);

/// @brief Returns true if FreeDV is currently active (any mode != OFF).
bool freedv_is_active(void);
