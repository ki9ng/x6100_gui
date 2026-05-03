/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA Nearby dialog — GPS-sorted park list.
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#pragma once

#include "dialog.h"

extern dialog_t *dialog_pota_nearby;

/**
 * Returns the park reference chosen by the user (MFK press), or NULL if the
 * dialog was dismissed without a selection. Valid until the next
 * dialog_construct(dialog_pota_nearby, ...) call.
 */
const char *pota_nearby_get_selected(void);
