/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA park history — persisted to pota_parks table in params.db
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 *
 *  Schema (created by migration _4):
 *    CREATE TABLE pota_parks (
 *        id    INTEGER PRIMARY KEY AUTOINCREMENT,
 *        park  TEXT NOT NULL UNIQUE,
 *        ts    INTEGER NOT NULL DEFAULT 0   -- unix epoch, most recent spot
 *    );
 */

#include "pota_parks.h"
#include "params/db.h"

#include <lvgl/lvgl.h>
#include <string.h>
#include <stdio.h>

/* In-memory cache, index 0 = most recent */
static char  parks[POTA_PARKS_MAX][16];
static int   parks_n = 0;

void pota_parks_init(void) {
    parks_n = 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT park FROM pota_parks ORDER BY ts DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("pota_parks_init: prepare failed: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, POTA_PARKS_MAX);

    while (sqlite3_step(stmt) == SQLITE_ROW && parks_n < POTA_PARKS_MAX) {
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        if (p) {
            strncpy(parks[parks_n], p, sizeof(parks[parks_n]) - 1);
            parks[parks_n][sizeof(parks[parks_n]) - 1] = '\0';
            parks_n++;
        }
    }
    sqlite3_finalize(stmt);
    LV_LOG_USER("pota_parks_init: loaded %d parks", parks_n);
}

int pota_parks_count(void) {
    return parks_n;
}

const char *pota_parks_get(int i) {
    if (i < 0 || i >= parks_n) return NULL;
    return parks[i];
}

void pota_parks_add(const char *park) {
    if (!park || park[0] == '\0') return;

    /* Upsert into DB — update ts if exists, insert if new */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO pota_parks(park, ts) VALUES(?, strftime('%s','now')) "
        "ON CONFLICT(park) DO UPDATE SET ts = strftime('%s','now')",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LV_LOG_ERROR("pota_parks_add: prepare failed: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, park, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Trim to POTA_PARKS_MAX oldest entries */
    sql_query_exec(
        "DELETE FROM pota_parks WHERE id NOT IN (SELECT id FROM pota_parks ORDER BY ts DESC LIMIT 10)"
    );

    /* Rebuild in-memory cache */
    pota_parks_init();
}
