# X6100 GUI — KI9NG Fork

[![License: LGPL v2.1+](https://img.shields.io/badge/License-LGPL%20v2.1%2B-blue.svg)](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
[![Hardware: Xiegu X6100](https://img.shields.io/badge/Hardware-Xiegu%20X6100-orange.svg)](https://www.xiegu.eu/x6100/)

An alternative firmware for the [Xiegu X6100](https://www.xiegu.eu/x6100/) HF transceiver, forked from [`gdyuldin/x6100_gui`](https://github.com/gdyuldin/x6100_gui) (the r1cbu community firmware). This fork adds features for POTA activators, JS8 digital-mode operations, and integration with on-radio decision support — built and tested by **KI9NG** at the workbench, not a corporate lab.

If you spotted this at Hamvention: hi 👋

---

## What this fork adds

### POTA self-spotting — WiFi + RF

The original firmware can spot to [pota.app](https://pota.app) over WiFi. This fork adds a **second spotting path over RF** using JS8 digital mode:

- **WiFi path** — instant HTTP POST to pota.app (works when you have data)
- **JS8 path** — encodes the spot as a multi-frame `@APRSIS CMD :APSPOT` directed message, transmits it on a JS8 calling frequency, and any nearby JS8Call station with APRS-IS gating enabled relays it to APRS-IS, where the [APSPOT](https://apspot.radio) service posts it to pota.app — *no internet needed at the park*.

The unified park picker shows your recent activations alongside the nearest parks from a 50 000-entry on-device POTA database, sorted by distance from your GPS fix. Pick a park, pick a method (WiFi or JS8), pick a band, optionally ATU-tune, and send.

```
┌────────────────────────────────────────────────────────────┐
│  MFK: scroll   Press: pick park                            │
│  ── RECENT ──                                              │
│   US-0765    4.2 km   Indiana Dunes National Park          │
│   US-1011   18.7 km   Warren Dunes State Park              │
│  ── NEARBY ──                                              │
│   US-9421    1.3 km   South Haven Linear Park              │
│   US-4567   12.1 km   Spicer Lake Nature Preserve          │
│   ...                                                      │
└────────────────────────────────────────────────────────────┘
       │
       ▼ (pick US-0765)
┌────────────────────────────────────────────────────────────┐
│  Spot US-0765 via...                                       │
│  WiFi: not connected   (dimmed)                            │
│                                                            │
│       ┌──────────────┐      ┌──────────────────┐           │
│       │ Send via WiFi│      │ Send via JS8Call │           │
│       └──────────────┘      └──────────────────┘           │
└────────────────────────────────────────────────────────────┘
       │
       ▼ (pick JS8Call, then 20m, ATU on)
┌────────────────────────────────────────────────────────────┐
│  JS8 spot US-0765 — pick band                              │
│   [80m] [40m] [30m] [20m*]                                 │
│   [17m] [15m] [12m] [10m ]                                 │
│             [ Send ]                                       │
└────────────────────────────────────────────────────────────┘
       │
       ▼  TX: 4–5 JS8 frames over 60–75 sec on 14.078 MHz
   pota.app spot appears within ~30 sec
```

### libx6100js8 — JS8 protocol engine

A standalone shared library that encodes JS8-Normal frames into 48 kHz PCM, built on Robert Morris AB1HL's [`rtmrtmrtmrtm/fate`](https://github.com/rtmrtmrtmrtm/fate) and verified byte-for-byte interoperable with the reference [JS8Call](http://js8call.com) decoder.

Lives in its own repo: [`ki9ng/x6100-js8-engine`](https://github.com/ki9ng/x6100-js8-engine). Pulled in as a buildroot package; this firmware links against `libx6100js8.so.0` and calls it from `dialog_pota_spot.c`. The library is intentionally scheduler-free — it produces per-frame PCM only, and this firmware drives slot-aligned PTT cycles around each frame.

The same library will power upcoming text-messaging and emergency-beacon features (see [Roadmap](#roadmap)).

### POTA database with distance sort

Embedded SQLite database of ~50 000 POTA parks worldwide. The dialog ranks parks by great-circle distance from the current GPS fix — useful when you arrive somewhere and aren't 100% sure which park's footprint you're standing in.

The database is loaded lazily, falls back gracefully when there's no GPS fix (everything still works without it), and the "Refresh Nearby" function re-queries after the radio has moved.

### Other improvements over upstream

- **FreeDV POC** in `feature/freedv-poc-v0.1` (shelved due to network-wide RADEV1 migration — see [wiki writeup](https://github.com/ki9ng/x6100_gui/blob/feature/freedv-poc-v0.1) for the post-mortem).
- **SSB-mute audio bug fixed** — the X6100's Linux side has no working ALSA playback path; speaker volume is controlled exclusively via the STM32 over I²C. Documented in `src/audio.c` and the wiki page `radio/x6100-baseband-audio.md`.
- **POTA nearby refactor** — original code was a two-dialog hide-and-reuse architecture with a recurring use-after-free; rewritten as a single view-state machine (commit `ee8a5da`, −261 lines).

---

## What this fork is *not*

- Not a Xiegu official release. If you brick your radio, that's on you.
- Not a JS8Call replacement. The X6100's UI (2 encoders + 5 function keys, 800×480 screen, no keyboard) cannot host a keyboard-to-keyboard QSO interface. JS8 is used here for one-shot directed messages — POTA spots today, text messages and emergency beacons coming.
- Not a fork of Aether's firmware. This is downstream of `gdyuldin/x6100_gui` which is downstream of `r1cbu`.

---

## Hardware target

| | |
|---|---|
| Radio | [Xiegu X6100](https://www.xiegu.eu/x6100/) HF transceiver |
| SoC | Allwinner A33 (dual Cortex-A7 @ 1.0 GHz) |
| Display | 800×480 px, **no touchscreen** — input is encoders + 5 function keys |
| RAM / Storage | 512 MB / SD card boot |
| Audio path | A33 → STM32 SSB modulator → DAC → PA; no working ALSA playback on the Linux side |

---

## Installing the firmware

### Option A — burn an SD card image (recommended for first time)

Download `sdcard.img` from the [upstream Releases page](https://github.com/gdyuldin/x6100_gui/releases/latest) (this fork doesn't yet publish full images). Flash with [balenaEtcher](https://www.balena.io/etcher/) or Rufus. Insert and boot.

Then SSH in (the radio runs an OpenSSH server on its WiFi/USB-CDC interface, default root password `123`) and overlay this fork's binary — see Option B below.

### Option B — overlay a freshly-built binary

You need this fork's `x6100_gui` binary plus `libx6100js8.so.0.3.0` (the JS8 engine library). Build instructions are in [Building from source](#building-from-source); the binaries land in `~/x6100_gui/buildroot/build/src/x6100_gui` and `~/AetherX6100Buildroot/build/target/usr/lib/libx6100js8.so.0.3.0`.

```bash
RADIO=192.168.1.231   # your radio's IP — connect to its WiFi or USB-CDC ethernet first

# Make the rootfs writable
sshpass -p 123 ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
    root@$RADIO 'mount -o remount,rw /'

# Back up the current binary (recommended)
sshpass -p 123 ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
    root@$RADIO 'cp /usr/sbin/x6100_gui /usr/sbin/x6100_gui.bak'

# Copy the new binary + JS8 engine library
sshpass -p 123 scp -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
    x6100_gui              root@$RADIO:/usr/sbin/x6100_gui.new
sshpass -p 123 scp -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
    libx6100js8.so.0.3.0   root@$RADIO:/usr/lib/libx6100js8.so.0.3.0

# Atomic swap + link refresh, then bounce the GUI (auto-restarts)
sshpass -p 123 ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no root@$RADIO '
    cd /usr/lib && ln -sf libx6100js8.so.0.3.0 libx6100js8.so.0 && ln -sf libx6100js8.so.0 libx6100js8.so
    mv /usr/sbin/x6100_gui.new /usr/sbin/x6100_gui
    killall x6100_gui  # systemd / init respawns it
'
```

The radio screen will go black for ~3 sec and come back with the new firmware. Push unstripped binaries during development — symbols are 100 KB extra but make crash post-mortems tractable.

### Reverting if something is broken

```bash
sshpass -p 123 ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
    root@$RADIO 'mv /usr/sbin/x6100_gui.bak /usr/sbin/x6100_gui && killall x6100_gui'
```

---

## Building from source

### Prerequisites

A reasonably modern Linux box (tested on Ubuntu 22.04 / 24.04). The buildroot toolchain pulls ~7 GB of sources on first run and builds a complete ARMv7 cross-toolchain — plan for 30–60 minutes of unattended compile time the first time.

```bash
sudo apt install build-essential git cmake bison flex \
                 libncurses-dev libssl-dev rsync wget unzip python3
```

### Clone the repos

```bash
mkdir x6100 && cd x6100

# Buildroot tree (provides the cross-toolchain + libx6100js8 buildroot package)
git clone https://github.com/gdyuldin/AetherX6100Buildroot

# This firmware
git clone https://github.com/ki9ng/x6100_gui
cd x6100_gui
git checkout feature/pota-nearby-unified   # current dev branch
git submodule update --init --recursive
cd ..
```

You will also need the KI9NG buildroot overlay that pins `libx6100js8` as a target package. If you don't have it locally, copy the package definition from this fork's wiki or grab it from `ki9ng/x6100-js8-engine/buildroot/`.

### Build the toolchain + base system

```bash
cd AetherX6100Buildroot
git submodule update --init
./br_config.sh
cd build
make -j$(nproc)
```

This is the slow one. Grab coffee.

### Build libx6100js8

```bash
make libx6100js8
```

Installs the .so into the toolchain staging sysroot so `x6100_gui` can link against it.

### Build x6100_gui

```bash
cd ../../x6100_gui
export PATH=$HOME/x6100/AetherX6100Buildroot/build/host/bin:$PATH
cd buildroot
./build.sh
```

The compiled ARMv7 binary lands at `buildroot/build/src/x6100_gui`. Deploy via the steps in [Installing the firmware](#installing-the-firmware).

---

## Daily-driver setup tips

### Importing past QSOs to mark worked callsigns

The firmware can highlight callsigns you've already worked. Copy your ADIF log to the `DATA` partition of the SD card as `incoming_log.adi`; on next boot it imports and renames the file to `incoming_log.adi.bak`. The `DATA` partition is created on first boot.

### Exporting FT8 QSOs

FT8/FT4 QSOs are appended to `ft_log.adi` on the `DATA` partition. Standard ADIF — feed it to QRZ, LoTW, or your favourite logger.

### POTA database

Worldwide POTA park dataset is embedded in the firmware (~50 000 parks). Updates ship with new releases. If you want to refresh it independently, the database is read from the SD card's `DATA` partition; the on-radio `pota_db_*` API is in `src/pota_db.c`.

---

## Repository structure

```
x6100_gui/
├── src/
│   ├── dialog_pota_spot.c     — unified RECENT+NEARBY picker, JS8 + WiFi spotting
│   ├── pota_spot.c            — WiFi POST to pota.app
│   ├── pota_db.c              — embedded SQLite POTA database
│   ├── pota_parks.c           — recent activations cache
│   ├── dialog_ft8.c           — FT8/FT4 (upstream, lightly modified)
│   ├── dialog_freedv.c        — FreeDV POC (shelved)
│   ├── audio.c                — PulseAudio glue + SSB-mute fix
│   ├── radio.c                — VFO/PTT/ATU state machine
│   └── ...
├── buildroot/                 — wrapper that drives cmake + cross-toolchain
├── lvgl/                      — LVGL 8.x submodule (UI framework)
└── third-party/               — FT8 library, codec2, etc.
```

Key subsystems worth knowing about for contributors:

- **Subject pattern** — the firmware uses a custom observer/subscribe pattern (`subject_t`) for radio state propagation. See `src/cfg/subject.h`. When you change radio state, **call `subject_set_int(cfg_cur.xxx, val)` not the raw `x6100_control_*` setter** — otherwise the observer chain doesn't fire and the UI lies to you.
- **Dialog lifecycle** — every full-screen mode is a `dialog_t` with `construct_cb` / `destruct_cb` / `key_cb` / optional `rotary_cb` and `audio_cb`. Don't store widget pointers in static globals — they outlive the dialog. Re-look them up on each construct.
- **MFK vs VFO encoders** — VOL/MFK is the top knob, VFO is the bottom knob (yes, naming is confusing). MFK rotation sends `KEY_VOL_LEFT_*` / `KEY_VOL_RIGHT_*`; MFK press sends `LV_KEY_ENTER`. VFO rotation goes through the rotary_cb.
- **No touchscreen** — the X6100 has *no* touchscreen. Every UI interaction is encoder or F-key. If you're tempted to add an `lv_btn` you can only "tap" via the encoder edit-mode dance — consider an F-key button instead (see `buttons.cpp`).

---

## Roadmap

| Feature | Status |
|---|---|
| Unified POTA park picker (RECENT + NEARBY) | ✅ Shipped |
| WiFi POTA spotting | ✅ Shipped (inherited from upstream) |
| JS8 POTA spotting via APSPOT | ✅ Implemented, field-test pending |
| Slot-aligned per-frame TX scheduler | ✅ Implemented (mirrors FT8 dialog) |
| ATU pre-tune before JS8 TX | ✅ Implemented |
| JS8 receive / decode (ACK display) | 🔧 Engine-side groundwork (Phase 4 of the engine project) |
| JS8 text messaging (prestored + arbitrary callsign) | 📋 Designed |
| Emergency beacon (`@ALLCALL` + SMS-via-APRS) | 📋 Designed |
| APRS position beacon | 📋 Designed |
| SSTV receive | 📋 Designed (see wiki `projects/x6100-sstv.md`) |
| FreeDV | ❌ Shelved (RADEV1 network migration; the X6100's libcodec2 can't decode it and an ARMv7 NEON port is multi-week work) |

---

## Wiki & deep-dive documentation

A working KI9NG-maintained wiki covers the design decisions, debugging stories, and hardware reverse-engineering behind this fork. Highlights:

- **`radio/js8call-pota-spot.md`** — design + 4-bug audit for the JS8 spot path
- **`radio/js8call-pota-spot-findings.md`** — APSPOT live-test findings (wire format, common failure modes)
- **`radio/x6100-baseband-audio.md`** — STM32 audio path reverse-engineering
- **`radio/x6100-lvgl-list-widgets.md`** — every LVGL list pattern in this firmware, with worked code examples
- **`radio/x6100-pota-nearby-ux-plan.md`** — the design rewrite that killed the 261-line second dialog
- **`projects/x6100-js8-engine.md`** — the libx6100js8 library project page

Some pages may move into this repo as `docs/` over time.

---

## Related projects

- [`ki9ng/x6100-js8-engine`](https://github.com/ki9ng/x6100-js8-engine) — the JS8 protocol engine library (libx6100js8). This firmware links against it.
- [`gdyuldin/x6100_gui`](https://github.com/gdyuldin/x6100_gui) — upstream of this fork (r1cbu community firmware).
- [`gdyuldin/AetherX6100Buildroot`](https://github.com/gdyuldin/AetherX6100Buildroot) — buildroot tree.
- [`AetherRadio/AetherX6100`](https://github.com/AetherRadio/AetherX6100) — the underlying X6100 hardware control library (libaether_x6100_control).
- [`rtmrtmrtmrtm/fate`](https://github.com/rtmrtmrtmrtm/fate) — Robert Morris AB1HL's minimal C++ JS8 implementation; the LDPC + FSK pipeline in libx6100js8 is based on this.
- [APSPOT](https://apspot.radio) — APRS-IS POTA spotting service used for the RF spot path.

---

## License

LGPL-2.1-or-later (per upstream). The linked-in `libx6100js8` is GPL-3.0-or-later; the combined on-radio binary is effectively GPL-3.

---

## Contact

- **Author**: Bill, KI9NG ([QRZ](https://www.qrz.com/db/KI9NG))
- **AllStar node**: 604010
- **Catch me at Hamvention**, Northern Indiana Communications Association (NICA) area.
- **Issues / PRs**: please open against this repo; tag `@ki9ng`.
