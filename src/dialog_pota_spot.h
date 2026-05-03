/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot dialog
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#pragma once

#include "dialog.h"
#include "buttons.h"

extern dialog_t *dialog_pota_spot;

/**
 * Called by dialog_pota_nearby when it closes after a selection (or ESC).
 * Destructs the hidden spot dialog and re-constructs it fresh so the
 * newly-added park appears at the top of the history list.
 */
void dialog_pota_spot_return(void);
