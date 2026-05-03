/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA parks binary database — load and nearest-park query.
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#include "pota_db.h"

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── binary format ─────────────────────────────────────────────────────── */

#define MAGIC       "PARK"
#define HDR_SIZE    16
#define ENTRY_SIZE  36  /* ref[11] + lat_udeg i32 + lon_udeg i32 + name[17] */

typedef struct __attribute__((packed)) {
    char    ref[11];
    int32_t lat_udeg;
    int32_t lon_udeg;
    char    name[17];
} raw_entry_t;

/* ── module state ──────────────────────────────────────────────────────── */

static pota_db_entry_t *db      = NULL;
static int              db_n    = 0;
static bool             loaded  = false;

/* ── distance ──────────────────────────────────────────────────────────── */

/* Equirectangular approximation — accurate enough for sorting within ~500 km */
static float dist_km(double lat1, double lon1, double lat2, double lon2) {
    const double R    = 6371.0;
    const double dlat = (lat2 - lat1) * M_PI / 180.0;
    const double dlon = (lon2 - lon1) * M_PI / 180.0;
    const double mlat = (lat1 + lat2) * 0.5 * M_PI / 180.0;
    const double x    = dlon * cos(mlat);
    return (float)(R * sqrt(x * x + dlat * dlat));
}

/* ── qsort comparator (set per-query via dist_km already in entry) ─────── */

static int cmp_dist(const void *a, const void *b) {
    float da = ((const pota_db_entry_t *)a)->dist_km;
    float db = ((const pota_db_entry_t *)b)->dist_km;
    return (da > db) - (da < db);
}

/* ── load ──────────────────────────────────────────────────────────────── */

static bool try_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char hdr[HDR_SIZE];
    if (fread(hdr, 1, HDR_SIZE, f) != HDR_SIZE) { fclose(f); return false; }

    if (memcmp(hdr, MAGIC, 4) != 0) {
        LV_LOG_WARN("pota_db: bad magic in %s", path);
        fclose(f);
        return false;
    }

    uint32_t count;
    memcpy(&count, hdr + 8, 4);  /* little-endian count at offset 8 */

    if (count == 0 || count > 200000) {
        LV_LOG_WARN("pota_db: suspicious count %u in %s", count, path);
        fclose(f);
        return false;
    }

    pota_db_entry_t *entries = malloc(count * sizeof(pota_db_entry_t));
    if (!entries) { fclose(f); return false; }

    raw_entry_t raw;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (fread(&raw, 1, ENTRY_SIZE, f) != ENTRY_SIZE) break;

        /* copy ref — ensure null termination */
        memcpy(entries[i].ref, raw.ref, POTA_DB_REF_LEN - 1);
        entries[i].ref[POTA_DB_REF_LEN - 1] = '\0';

        /* convert microdegrees to degrees */
        entries[i].lat = raw.lat_udeg / 1e6;
        entries[i].lon = raw.lon_udeg / 1e6;

        /* copy name */
        memcpy(entries[i].name, raw.name, POTA_DB_NAME_LEN - 1);
        entries[i].name[POTA_DB_NAME_LEN - 1] = '\0';

        entries[i].dist_km = 0.0f;
    }

    fclose(f);

    if (db) free(db);
    db     = entries;
    db_n   = (int)i;
    loaded = true;
    LV_LOG_USER("pota_db: loaded %d parks from %s", db_n, path);
    return true;
}

bool pota_db_load(void) {
    if (loaded) return true;
    if (try_load("/mnt/DATA/pota-parks.bin")) return true;
    if (try_load("/usr/share/x6100/pota-parks.bin")) return true;
    LV_LOG_WARN("pota_db: no park database found");
    return false;
}

bool pota_db_ready(void) { return loaded && db_n > 0; }
int  pota_db_count(void) { return db_n; }

/* ── nearest query ─────────────────────────────────────────────────────── */

int pota_db_nearest(double lat, double lon, pota_db_entry_t *out, int n) {
    if (!loaded || db_n == 0 || !out || n <= 0) return 0;

    /* Stamp distances into the master array then partial-sort.
     * For 88k parks this is ~3 ms on the A33 — acceptable. */
    for (int i = 0; i < db_n; i++)
        db[i].dist_km = dist_km(lat, lon, db[i].lat, db[i].lon);

    /* Full sort — we want the user to be able to scroll the whole list */
    qsort(db, db_n, sizeof(pota_db_entry_t), cmp_dist);

    int give = (n < db_n) ? n : db_n;
    memcpy(out, db, give * sizeof(pota_db_entry_t));
    return give;
}
