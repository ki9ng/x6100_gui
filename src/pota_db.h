/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA parks binary database — load and nearest-park query.
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 *
 *  Binary format (little-endian, produced by scripts/fetch-pota-parks.py):
 *    header  16 bytes: magic "PARK" | version u32 | count u32 | epoch u32
 *    entry   36 bytes: ref[11] | lat_udeg i32 | lon_udeg i32 | name[17]
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define POTA_DB_REF_LEN  11
#define POTA_DB_NAME_LEN 17

typedef struct {
    char    ref[POTA_DB_REF_LEN];   /* null-terminated, e.g. "K-1234\0" */
    double  lat;                     /* degrees */
    double  lon;                     /* degrees */
    char    name[POTA_DB_NAME_LEN];  /* null-terminated, truncated */
    float   dist_km;                 /* filled in by pota_db_nearest() */
} pota_db_entry_t;

/**
 * Load the binary park database from disk.
 * Tries /mnt/DATA/pota-parks.bin first, then /usr/share/x6100/pota-parks.bin.
 * Returns true on success. Safe to call multiple times (no-op if loaded).
 */
bool pota_db_load(void);

/**
 * Return true if the database is loaded and has at least one park.
 */
bool pota_db_ready(void);

/**
 * Return total number of parks in the database.
 */
int pota_db_count(void);

/**
 * Fill `out` with up to `n` nearest parks to (lat, lon), sorted by distance.
 * Returns actual number of results written (may be less than n).
 * pota_db_load() must have been called first.
 */
int pota_db_nearest(double lat, double lon, pota_db_entry_t *out, int n);
