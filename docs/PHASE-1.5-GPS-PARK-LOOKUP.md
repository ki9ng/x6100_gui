# Phase 1.5: GPS-Assisted Park Selection — Design Notes

**Status**: Not started. Design captured 2026-04-20 for implementation at a later date.

**Prerequisite**: Phase 1 (WiFi spot) shipped in `pota-spot-v0.1-20260420-0254`.

---

## Problem

Activators often know roughly where they are but have to look up or remember the exact POTA reference (`K-1234`, `US-2198`, etc). Typing a reference on the X6100's knob-driven on-screen keyboard is slow. POTA references aren't memorable — they're arbitrary numbers.

An activator with a USB GPS puck attached should be able to see a list of nearby parks and pick one with the rotary, instead of typing the reference character by character.

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
- No new threading primitives beyond what `pota_spot.c` already has (which is: none). WiFi POST is sub-second, blocking inline is fine.
- cJSON not currently in the build. Either add it (`BR2_PACKAGE_CJSON=y`) or hand-parse the response.

---

## What's already in the firmware (don't duplicate)

- `src/gps.c` — background thread connects to `gpsd`, maintains a live `gpsdata` struct. Use from any other thread — snapshot-copy under a lock.
- `src/gps.h` — exposes `gps_init()` (called at startup) and `gps_status()` → `{WAITING, WORKING, RESTARTING, EXITED}`.
- `src/dialog_gps.c` — example of how to read live GPS (`fix.latitude`, `fix.longitude`, `fix.mode`, satellite counts) and update the UI.
- `src/wifi.h` — `wifi_get_status() == WIFI_CONNECTED` check already used by `pota_spot_wifi()`.
- `src/textarea_window.c` — knob-navigated on-screen keyboard (what Phase 1 uses for park entry).
- `lv_dropdown_create()` and `lv_list_create()` both work with keypad input — `dialog_settings.cpp` uses dropdowns the same way this feature would use a list.

---

## UX (knob-only)

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

- `Nearby` — always visible. Behavior when pressed depends on state (see "enable vs. lenient" below).
- `Manual` — identical to current Phase 1 `Park #` button. Opens the on-screen keyboard.
- `SPOT` — unchanged.
- `CANCEL` — unchanged.

GPS status line is a read-only label updated on a 500ms timer (same pattern as `dialog_gps.c`).

### Nearby list screen

```
┌─ Nearby POTA Parks ──────────────────┐
│  ▶ K-4245  Indiana Dunes NP    0.3km │
│    K-2257  Indiana Dunes SP    1.8km │
│    K-6732  Cowles Bog Trail    2.1km │
│    K-8841  Miller Woods        4.4km │
│    K-4422  Hoosier Prairie SNP 5.9km │
│    ...                               │
│                                      │
│  Rotate to scroll · Select to pick   │
└──────────────────────────────────────┘
                         [ BACK ]
```

- `lv_list_create()` with one `lv_list_add_btn()` per park. Container holds the list.
- Rotary scrolls the focus; select key picks the focused row.
- Maybe 10-15 results. Truncate names with `LV_LABEL_LONG_DOT` if needed.
- Selecting a row: `park_buf` populated, switch back to `cont_input`, focus goes to `SPOT` button.
- `BACK` returns to input screen without changing the park.
- If API call fails or returns empty, show an error line instead of the list.

### Lenient vs. strict enable on `Nearby`

**Decision: lenient.** Always render the button enabled. Clicking it when the preconditions aren't met shows an explanatory message:

- No GPS puck / no fix: `"No GPS fix. Attach a USB GPS puck and wait for a 2D+ fix. See APP → GPS for status."`
- No WiFi: `"Nearby lookup needs WiFi. Connect WiFi or use Manual entry."`
- Both missing: show GPS message first (GPS is the unusual hardware; WiFi they already know about).

Reasoning: a greyed-out button is confusing — the operator doesn't know what to do next. A lenient button with a clear error message is self-teaching.

---

## Data source

### Option A: POTA online API (preferred for Phase 1.5)

The pota.app frontend already does nearby-park lookups as the user pans the map. The endpoint is not documented but is discoverable.

**Open research action**: open pota.app in a desktop browser with devtools → Network tab open. Pan and zoom the map. Watch for the XHR that fetches parks. Capture:
- Full URL pattern (base + query params)
- Request headers (any special origin/referer needed?)
- Response JSON shape (fields for reference, name, lat, lon)

Likely candidates to check:
```
GET https://api.pota.app/locations/parks?latitude=...&longitude=...&radius_km=...
GET https://api.pota.app/park/autocomplete?q=...
GET https://api.pota.app/park/search?bbox=...
```

If no suitable endpoint exists, fall back to Option B.

### Option B: Offline parks database (fallback; also useful for Phase 2)

Download the POTA parks CSV (~80k rows, ~10-15 MB) at firmware build time, ship on the SD card. On the radio, sorted-by-centroid-distance search is trivial.

Pros:
- Works without WiFi — critical once Phase 2 (SOTAmat) is shipping for truly remote activations
- Independent of POTA API availability and endpoint stability
- Fast — microseconds

Cons:
- Stale data; new parks not known until firmware update or manual refresh
- Needs a refresh mechanism ("Update parks DB" button in Settings that downloads the CSV when WiFi is available)
- SD card space cost

Defer until after Phase 1.5 ships with the online endpoint.

---

## JSON parsing

Firmware currently builds JSON with `snprintf` but doesn't parse any incoming JSON. For this feature, pick one:

1. **Add cJSON** — single-header, ~3kB. `BR2_PACKAGE_CJSON=y` in buildroot. Cleanest; reusable for future features.
2. **Hand-roll** — fragile, but avoids the dependency. Response shape is predictable so doable.

Prefer cJSON.

---

## Implementation sketch

### New files

```
src/pota_parks.h
src/pota_parks.c
```

Interface:

```c
// pota_parks.h
typedef struct {
    char   reference[12];   // "K-1234", "US-2198", up to ~10 chars
    char   name[64];
    double distance_km;
} pota_park_t;

// Blocking HTTP call, up to ~1-2 sec on good WiFi.
// Returns number of parks found (0 on empty/error). Fills out[] up to max_results.
int pota_find_nearby(double lat, double lon,
                     pota_park_t *out, int max_results);
```

### Modified files

**`src/dialog_pota_spot.c`:**
- Add fourth container: `cont_nearby_list`
- Add button: `btn_nearby` (pressed → check GPS+WiFi → call `pota_find_nearby` → populate list → show)
- Add callback: `park_selected_cb` (list row selected → copy reference to `park_buf` → show input screen)
- Update GPS status line on input screen via a timer
- Page changes:
  - `page_input`: `{ &btn_nearby, &btn_manual, &btn_spot, &btn_cancel }` (was 3 buttons; now 4)
  - `page_nearby`: `{ &btn_back }`

**`src/CMakeLists.txt`:** add `pota_parks.c`

**`br2_external/configs/X6100_defconfig`** (in AetherX6100Buildroot): add `BR2_PACKAGE_CJSON=y` if going cJSON route.

### Estimated size

- `pota_parks.c`: ~100-150 lines (HTTP call + JSON parse + distance math)
- `dialog_pota_spot.c` additions: ~100-150 lines (new container, list callbacks, button logic)
- Total: ~300 lines of new code, no new threading, no new external dependencies beyond cJSON.

### Estimated time

Half a day of coding once the POTA endpoint is captured. The unknown is the endpoint signature — if it takes an hour of devtools sniffing to figure out, fine. If it needs reverse engineering a compiled JS bundle, longer.

---

## Why not do it right now

Phase 1 hasn't been tested on actual hardware yet. Build is clean, release is published, but the SD card hasn't been flashed and booted. Until we confirm:

- The dialog renders correctly on the 800×480 screen
- The POTA button shows up at APP → page 3
- Knob navigation actually reaches the button
- WiFi POST works from the radio (not just from the OptiPlex)

...adding more features on top is building on an unproven foundation. Phase 1.5 should come after Phase 1 is confirmed working.

---

## Open questions when implementation starts

1. POTA nearby-parks endpoint signature (sniff pota.app)
2. cJSON vs hand-rolled parser
3. How to handle the GPS status update — LVGL timer at 500ms (like `dialog_gps.c`) or one-shot read when the dialog opens?
4. How many results in the list? 10? 20? All within 10km?
5. What's the sort order — distance only, or distance + recent activation count (hot parks first)?
6. Should the selected park reference persist across dialog closes as the "last used" default (like WiFi SSID does)?

---

## Reference implementation hints

The tightest-fit reference in the existing code is `dialog_settings.cpp`'s long-press action dropdown:
- Uses `lv_dropdown_create()` — very similar UX pattern to `lv_list_create()`
- Rotary-navigable without any special handling
- `lv_dropdown_get_selected()` reads the picked index

For the list widget itself, LVGL 8.x docs: https://docs.lvgl.io/8.3/widgets/extra/list.html
