/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI — FreeDV 700D integration (ki9ng fork)
 */

#pragma once

#include <stdbool.h>

/* Application-level mode enum.
 * Uses FDV_ prefix to avoid collision with codec2's FREEDV_MODE_* macros. */
typedef enum {
    FDV_MODE_OFF  = 0,
    FDV_MODE_700D = 1,
    FDV_MODE_1600 = 2,
} freedv_mode_t;

void freedv_init(void);
void freedv_deinit(void);

/* fdv_get_mode / fdv_set_mode use the fdv_ prefix to avoid collision with
 * codec2's own freedv_get_mode(struct freedv *) function signature. */
freedv_mode_t fdv_get_mode(void);
void          fdv_set_mode(freedv_mode_t mode);

const char *freedv_mode_label(freedv_mode_t mode);
bool        freedv_is_active(void);

/* Called from main_screen_notify_rx_tx() on PTT transitions */
void freedv_on_tx_start(void);
void freedv_on_tx_stop(void);
