# Phase 1.5: GPS-Assisted Park Selection — Design Notes

**Status**: Not started. Design captured 2026-04-20 for implementation at a later date.

**Prerequisite**: Phase 1 (WiFi spot) shipped in `pota-spot-v0.1-20260420-0254`.

---

## Problem

Activators often know roughly where they are but have to look up or remember the exact POTA reference. Typing a reference on the X6100's knob-driven on-screen keyboard is slow. POTA references aren't memorable — they're arbitrary numbers.

An activator with a USB GPS puck attached should be able to see a list of nearby parks and pick one with the rotary, instead of typing the reference character by character.

Critically: this should work **without WiFi at the activation site** — that's the common remote-activation case. WiFi is needed to *update* the database, not to use it.

---

## Constraints (don't forget these)

### Hardware

- **X6100 has no built-in GPS.** GPS is only available when the operator plugs in a USB GPS puck. The firmware talks to it via `gpsd` on `localhost:2947`.
- **No touch screen. No mouse. No keyboard.** Input is rotary encoders and hardware buttons only. LVGL sees these as `LV_INDEV_TYPE_KEYPAD` / `LV_INDEV_TYPE_ENCODER` events.
- Screen is 800×480. Dialog area is roughly 400×280 based on Phase 1 measurements.

### Policy

- **Do not verify the operator is actually inside the park.** POTA rules put that responsibility on the operator. The firmware's job is to show what's nearby; the operator decides.
- Don't auto-submit a spot just because GPS says they're "in" a park. Every spot requires an explicit `SPOT` button press.
- Don't filter out parks the operator isn't "in" — they may be setting up just outside a boundary, activating an overlap, etc. Show by distance, let them pick.

### Code

- Must match existing firmware patterns: `dialog_t` descriptor, `buttons_page_t` pages, rotary-navigable LVGL widgets.
- No new threading primitives beyond what `pota_spot.c` already has, EXCEPT for the DB update operation (that's legitimately long-running).

---

## Data sizing — REAL NUMBERS (measured 2026-04-20)

Fetched every park worldwide from the POTA API:

| | |
|---|---|
| Total parks worldwide | **88,740** |
| Countries represented | 235 |
| Raw JSON from API | 16.2 MB |
| Minimal CSV (ref, lat, lon, name) | 5.4 MB |
| **Packed binary (36 B/park)** | **3.05 MB** |
| Packed binary gzipped | 1.5 MB |

Top 10 by park count: US 12,921 · FR 12,816 · AU 11,166 · GB 6,527 · CA 5,988 · NO 3,334 · PL 3,089 · ES 2,597 · IT 2,095 · SE 1,983

**Decision: ship worldwide, 3 MB on SD card is nothing.** Regional filtering is unnecessary given these numbers.

Sample dump committed at `scripts/sample-output/world.bin.sample` for reference.

---

## API endpoints verified

| Endpoint | What it returns | Size |
|----------|----------------|------|
| `GET /programs` | Full list of 264 POTA programs (countries/territories) | ~60 KB |
| `GET /locations?program=US` | All 3,770 subdivisions worldwide (yes, query param is ignored) | 676 KB |
| `GET /locations/<ISO>` | All parks in one subdivision (e.g. `US-IN` → 230 parks) | ~1-50 KB each |
| `GET /park/<ref>` | Full detail for one park (name, polygon, grid, stats) | ~800 B each |

All are public, no auth. Cached at CloudFront (`max-age=3600`).

For a full world refresh: 3,770 API calls averaging ~15 KB each = **~55 MB raw, ~15 MB gzipped**. Takes ~3-8 minutes depending on connection.

---

## On-radio binary format

```c
// File: /mnt/DATA/pota-parks.bin
// Little-endian.

struct parks_header {
    char     magic[4];       // "PARK"
    uint32_t version;        // 1
    uint32_t count;          // number of entries
    uint32_t epoch;          // unix timestamp of last refresh
};  // 16 bytes

struct park_entry {
    char     ref[11];        // "US-0288", null-padded, max 10 ASCII chars + null
    int32_t  lat_udeg;       // latitude × 1e6, fits ±90° in int32
    int32_t  lon_udeg;       // longitude × 1e6
    char     name[17];       // first 16 UTF-8 chars of name + null
} __attribute__((packed));   // 36 bytes

// Entries sorted by reference for deterministic builds.
// Total: 16 + 36 × N bytes  →  88,740 parks = 3.05 MB
```

Lookup is straightforward brute-force scan:
```c
int pota_find_nearby(double lat, double lon,
                     park_match_t *out, int max_results) {
    // Pre-compute cos(lat) once for equirectangular approximation
    const double lat_rad = lat * M_PI / 180.0;
    const double cos_lat = cos(lat_rad);
    const int32_t my_lat = (int32_t)(lat * 1e6);
    const int32_t my_lon = (int32_t)(lon * 1e6);

    // Walk mmap'd file, maintain top-N heap (or just sort at end)
    // 88k entries × ~15ns/entry = ~1.3 ms on A33 — fast enough
}
```

No k-d tree needed. 88k iterations of integer math plus a sort of the top N is <10ms on the A33.

Full park name (truncated at 16 chars in the binary) can be fetched via `GET /park/<ref>` when the user selects a row *and* WiFi is available — it's nice-to-have for confirmation but the ref is the real ID.

---

## UX (knob-only, offline-first)

### Input screen gains a `Nearby` button

```
┌─ POTA Self-Spot ─────────────────────┐
│                                      │
│  Park:  — not set —                  │
│  14.234 MHz  SSB                     │
│                                      │
│  GPS: 3D fix, 7 sats                 │  ← "GPS: no fix" or "GPS: not connected"
│                                      │
└──────────────────────────────────────┘
  [ Nearby ]  [ Manual ]  [ SPOT ]  [ CANCEL ]
```

- `Nearby` enabled requirement: **GPS fix only**. WiFi is NOT required — we have the DB onboard.
- `Manual` — unchanged from Phase 1
- `SPOT`, `CANCEL` — unchanged

GPS status line updated on a 500ms timer (pattern from `dialog_gps.c`).

### Nearby list screen

```
┌─ Nearby POTA Parks ──────────────────┐
│ ▶ US-4245  Indiana Dunes NP    0.3km │
│   US-2257  Indiana Dunes SP    1.8km │
│   US-6732  Cowles Bog Trail    2.1km │
│   US-8841  Miller Woods        4.4km │
│   US-4422  Hoosier Prairie     5.9km │
│   US-3305  Gary State Park    10.2km │
│   ...                                │
│                                      │
│  Rotate to scroll · Select to pick   │
└──────────────────────────────────────┘
                         [ BACK ]
```

- `lv_list_create()` with ~15 rows visible, total ~50 nearest parks returned
- Rotary scrolls focus; select picks
- Selecting populates park_buf, switches back to input screen, focus goes to `SPOT`
- Names shown are the truncated 16-char version from the binary; full name visible only after selection if online (optional enhancement)

### Lenient enable with explanatory errors

`Nearby` button always visually enabled. Pressing it when unavailable shows a message:
- No GPS fix: `"No GPS fix. Attach USB GPS puck and wait for fix. See APP → GPS."`
- DB missing (shouldn't happen in shipped firmware): `"Park database missing. Settings → POTA DB → Update Now"`

Reason: greyed-out buttons confuse users who don't know what's wrong.

---

## Database update UX

New Settings menu item: **POTA Database**.

```
┌─ POTA Park Database ─────────────────┐
│                                      │
│  Current: 88,740 parks               │
│  Updated: 2026-04-20 (today)         │
│                                      │
│  [ Update Now ]     [ Back ]         │
└──────────────────────────────────────┘
```

Pressing **Update Now**:

```
┌─ Updating Park Database ─────────────┐
│                                      │
│  Fetching: 1247 / 3770               │
│  ████████░░░░░░░░░░░░░░░             │
│  34,812 parks so far                 │
│                                      │
│               [ CANCEL ]             │
└──────────────────────────────────────┘
```

Flow:
1. Check WiFi → if not connected, error
2. Background thread (legitimate case — runs several minutes):
   - GET `/locations?program=US` → list of subdivisions
   - For each subdivision, GET `/locations/<ISO>`, append parks
   - Progress updates posted via `scheduler_put()` for the progress bar
3. Sort by reference, pack to binary
4. Write to `/mnt/DATA/pota-parks.bin.tmp`
5. Atomic `rename()` to `pota-parks.bin`
6. Update epoch, refresh mmap
7. Return to settings with new timestamp visible

Cancellable via CANCEL button (sets an atomic flag; worker stops at next subdivision boundary).

### Age indication

- Settings shows plain date
- If DB is > 90 days old, input dialog's GPS status line shows a subtle nag:  
  `"GPS: 3D fix, 7 sats · DB 127 days old"`

No auto-update. WiFi connection timing is user's call.

---

## Firmware-bundled initial database

Ship a `pota-parks.bin` inside the rootfs via buildroot so first boot has a working DB.

**Build-time flow** (in AetherX6100Buildroot):
1. Add a post-build script that runs `fetch-pota-parks.py` on the build host
2. Installs result to `$TARGET_DIR/usr/share/pota-parks.bin`
3. Adds ~3 MB to the rootfs

**First-boot logic** in firmware:
```c
if (access("/mnt/DATA/pota-parks.bin", R_OK) != 0) {
    // Copy bundled DB to persistent location
    copy_file("/usr/share/pota-parks.bin", "/mnt/DATA/pota-parks.bin");
}
```

This means even a fresh radio with no WiFi ever configured still has 88k parks available the moment a GPS puck is attached.

---

## Implementation sketch

### New files

```
src/pota_parks.h          /* public API */
src/pota_parks.c          /* mmap binary, find_nearby, update logic */
src/dialog_pota_db.c      /* Settings → POTA DB dialog */
src/dialog_pota_db.h
scripts/fetch-pota-parks.py  /* host-side prefetch for build + cli testing */
```

### Modified files

```
src/dialog_pota_spot.c    /* Add Nearby button, nearby list container */
src/dialog_settings.cpp   /* Add "POTA Database" menu item */
src/params/params.h       /* Add ACTION_APP_POTA_DB */
src/main_screen.c         /* Wire new action */
src/CMakeLists.txt        /* Add new sources */
```

### Buildroot changes

```
br2_external/configs/X6100_defconfig   /* BR2_PACKAGE_CJSON=y */
br2_external/package/pota-parks/       /* new package that installs pota-parks.bin */
```

### Rough sizing

- `pota_parks.c`: ~250 lines (mmap + find_nearby + background updater + atomic write)
- `dialog_pota_db.c`: ~150 lines (settings dialog with progress bar)
- `dialog_pota_spot.c` additions: ~100 lines (nearby button + list container)
- Total: ~500 lines of C + ~150 lines Python for the prefetch

### Update thread architecture

Updater runs in a detached pthread because it's many-minute long. Uses `scheduler_put()` to push progress updates back to the LVGL thread. Atomic cancel flag checked between each subdivision fetch.

```c
// pota_parks.c
static atomic_bool update_cancel;
static atomic_bool update_running;

typedef struct {
    int fetched;
    int total;
    int parks;
} update_progress_t;

void pota_parks_start_update(pota_update_progress_cb_t cb);
void pota_parks_cancel_update(void);
bool pota_parks_update_running(void);
```

---

## Action items (in order)

- [ ] Flash and test Phase 1 on actual hardware — confirm WiFi POST works from the radio
- [ ] Verify `cJSON` is addable to buildroot (or pick a hand-rolled alternative)
- [ ] Add `pota-parks` buildroot package that runs `fetch-pota-parks.py` at build time and installs the binary
- [ ] Implement `pota_parks.c` with mmap + find_nearby
- [ ] Implement `dialog_pota_db.c` (settings page with progress bar)
- [ ] Background updater thread with cancel + atomic rename
- [ ] `dialog_pota_spot.c`: add `Nearby` button + nearby list container
- [ ] GPS status line on input screen with 500ms timer
- [ ] First-boot copy of bundled DB to `/mnt/DATA/` if missing

---

## Not doing (captured to avoid scope creep)

- No polygon containment check
- No auto-submission of spots based on GPS position
- No regional-only DB variants (world is only 3 MB; no point splitting)
- No auto-update on WiFi connect — manual trigger only
- No delta updates (full refetch is ~15 MB gzipped; simpler than diff logic)
- No on-radio park name lookup beyond the truncated 16-char name; full name requires WiFi via `/park/<ref>`

---

## Reference: fetch-pota-parks.py

Committed at `scripts/fetch-pota-parks.py`. Fetches all 3,770 subdivisions, packs to 36-byte-per-park binary. Sample output from 2026-04-20: **88,740 parks, 3,194,672 bytes**.

Run ad-hoc for testing:
```bash
./scripts/fetch-pota-parks.py --output /tmp/parks.bin --csv /tmp/parks.csv
```
