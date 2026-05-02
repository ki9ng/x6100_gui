/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA park history — recently self-spotted parks, persisted to params.db
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define POTA_PARKS_MAX 10   /* how many recent parks to remember */

/**
 * Load recent parks from DB into an internal array.
 * Call once after database_init().
 */
void pota_parks_init(void);

/**
 * Return the number of stored parks (0–POTA_PARKS_MAX).
 */
int pota_parks_count(void);

/**
 * Return park reference string at index i (0 = most recent).
 * Returns NULL if i is out of range.
 */
const char *pota_parks_get(int i);

/**
 * Record a successfully spotted park.
 * Moves it to the front if already present; evicts oldest if full.
 * Persists immediately to params.db.
 */
void pota_parks_add(const char *park);
