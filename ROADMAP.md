# Roadmap — ki9ng fork

Feature plans specific to this fork. Base firmware features tracked upstream at [gdyuldin/x6100_gui](https://github.com/gdyuldin/x6100_gui).

## Shipped

### Phase 1 — POTA self-spot over WiFi

`APP → page 3 → POTA Spot` menu item. Operator enters park reference; firmware reads current freq + mode, posts a spot to `api.pota.app`. No RF transmission. Immediate confirmation via HTTP 200 response.

- Release: [`pota-spot-v0.1-20260420-0254`](https://github.com/ki9ng/AetherX6100Buildroot/releases/tag/pota-spot-v0.1-20260420-0254)
- Design notes: [this branch, post-merge](.)

## Planned

### Phase 1.5 — GPS-assisted park selection

When a USB GPS puck is connected and has a fix, offer a rotary-scrollable list of nearest POTA parks so the operator can pick instead of typing. No polygon verification — operator picks what they want to spot.

- Design: [`docs/PHASE-1.5-GPS-PARK-LOOKUP.md`](docs/PHASE-1.5-GPS-PARK-LOOKUP.md)
- Tracking issue: [#1](https://github.com/ki9ng/x6100_gui/issues/1)
- Prerequisite: Phase 1 tested on actual hardware
- Open unknowns: POTA nearby-parks API endpoint signature

### Phase 2 — FT8 free-text spotting via SOTAmat

For operators without WiFi at the activation site. Transmits a 13-character FT8 free-text message containing a personal suffix that SOTAmat's servers decode into a park reference. Currently blocked on SOTAmat's proprietary config blob format.

- Stub present in `src/pota_spot.c`
- Blocker: need SOTAmat config blob format; contact AB6D (sotamat@sotamat.com)
- Coverage limitation: only ~124 SparkSDR/CWSL_DIGI stations worldwide decode FT8 free-text

### Phase 3 — Native JS8Call encoding

Direct POTA spotting via JS8Call directed message (`@POTA DE KI9NG K-1234`). Coverage depends on JS8 relay stations. Lower priority than Phase 2 because JS8 adoption is smaller than FT8.

### Offline parks database

Ship the POTA parks CSV (~10-15 MB) on the SD card so Phase 1.5 works without WiFi. Most valuable once Phase 2 is shipping — combining offline spotting (FT8) with offline park lookup enables truly-remote activations.

## Design principles (for contributors)

- **X6100 has no touch screen, no mouse, no keyboard.** All UI must be navigable with rotary encoders and hardware buttons. LVGL sees input as `LV_INDEV_TYPE_KEYPAD` / `LV_INDEV_TYPE_ENCODER`.
- **X6100 has no built-in GPS.** GPS is optional via USB puck and `gpsd`. Don't block features behind GPS availability unless absolutely necessary.
- **Don't verify operator policy compliance.** POTA rules (being inside the park, operator eligibility, etc.) are the operator's responsibility. Firmware provides information; operator decides.
- **Blocking is fine for sub-second operations.** WiFi HTTP POSTs complete in ~200ms; threading adds more bugs than it prevents at that scale.
- **Follow existing patterns.** Look at `dialog_ft8.c`, `dialog_settings.cpp`, `dialog_gps.c` before inventing new structure. The codebase has a consistent style for dialog descriptors, button pages, and keypad event handling.
