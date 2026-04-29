# Roadmap — ki9ng FreeDV-only PoC fork

This roadmap covers the FreeDV branch of the ki9ng fork. The POTA self-spot
work lives on a separate branch (`feature/pota-spot`); see that branch for
its own roadmap. Base firmware features are tracked upstream at
[gdyuldin/x6100_gui](https://github.com/gdyuldin/x6100_gui).

## Why a separate "FreeDV-only" image

This is a proof of concept. The image is intended to be swapped in by SD
card when you want FreeDV; for normal radio operation, swap to a stock r1cbu
/ gdyuldin SD card and reboot. The X6100 always prefers SD over internal
flash, so the swap is just a power-cycle.

Future versions will fold FreeDV into a normal full-featured image with
internet features (current user list, etc.) similar to the desktop FreeDV
GUI.

## v0.1 — Boot-clean scaffold (this release)

- `codec2` vendored as a git submodule at `third-party/codec2` (tag
  [`1.2.0`](https://github.com/drowe67/codec2/releases/tag/1.2.0)).
  **Source is in tree but not yet linked into the build.**
- New module `src/freedv.{c,h}` — stub API: `freedv_init`,
  `freedv_set_mode`, `freedv_get_mode`, `freedv_is_active`,
  `freedv_mode_label`. No codec2 calls; no audio bridge.
- `freedv_init()` is wired into `src/main.c` after `params_init()`.
- Boot banner via existing `msg_schedule_long_text_fmt()` identifying the
  image as the FreeDV-only PoC.
- CI workflow auto-marks `freedv-poc-*` tag releases as **prereleases** so
  they don't surface as the latest stable image.

**Pass criteria:** image builds in CI, boots on the X6100, banner visible,
existing GUI features unaffected. **No FreeDV operation yet.**

## v0.2 — Link codec2 into the build

- Add codec2 as a buildroot package (or include via `add_subdirectory()`
  with `UNITTEST=OFF` and example programs disabled).
- `freedv.c` opens / closes `freedv_open()` from `<codec2/freedv_api.h>` for
  700D and 1600. Call it once at init to confirm the library actually loads
  and report `libcodec2` version to stderr.
- Settings panel exposes a mode dropdown (OFF / 700D / 1600). The setting
  persists via the params SQLite store.
- Status indicator on the info row: `FDV-OFF` / `FDV-700D` / `FDV-1600`.

**Pass criteria:** image still boots, mode setting persists across reboot,
codec2 init/teardown does not leak or crash.

## v0.3 — Real-time audio bridge

- Connect `alsa_input.platform-sound.stereo-fallback` (PulseAudio mic
  source) to `freedv_tx()` and write encoded audio to
  `alsa_output.platform-sound.stereo-fallback` (sink that reaches the
  STM32's SSB modulator via I²S).
- Symmetric RX path: PA capture → `freedv_rx()` → AF output.
- Sample-rate conversion 8 kHz ↔ 48 kHz via libsamplerate (if available
  in the buildroot rootfs; otherwise a simple linear interpolator).
- Encode/decode runs in a dedicated `pthread` — never block the LVGL UI
  thread.

**Pass criteria:** loopback test from mic → encoder → decoder → speaker
produces audible (degraded) speech with under 600 ms latency.

## v0.4 — UI lock-down

- When FreeDV mode is active, the radio is forced to L-DIG / U-DIG mode at
  TX keydown via `x6100_vfo_set_mode(MOD_USB_D)` /
  `MOD_LSB_D`, and restored on keyup.
- Mode-change controls are interlocked while FreeDV is active.
- TX bandpass filter widened to 300–2800 Hz to pass the FreeDV 700D modem
  signal without rolloff (uses the r9 BASE patch's user-controllable BPF).

**Pass criteria:** the operator cannot accidentally TX FreeDV in a non-DIG
mode; mode lock visible in the UI.

## v0.5 — On-air interop

- Test against the official PC FreeDV GUI on a second radio at the standard
  North American calling frequency (14.236 MHz USB for 700D — verify
  current convention against the FreeDV calling-frequency list).
- Confirm bidirectional decode (X6100 ↔ PC) for both 700D and 1600.
- Self-test via the OptiPlex's OpenWebRX as an independent receiver.

**Pass criteria:** voice copies cleanly both directions on a 100 W path.

## Future — Internet features

The desktop FreeDV GUI publishes a "current user list" so operators can
see who else is on the network. v0.6+ folds an equivalent feature into
the X6100 image, layered on top of the existing WiFi stack.

## Design principles

- **No STM32 changes.** FreeDV-on-SSB works through the existing L-DIG
  audio path — the STM32 sees ordinary SSB-band audio. STM32 patching is
  out of scope for this branch (and is the prerequisite for FreeDV-on-FM,
  which is a separate project).
- **No external hardware required.** Mic and speaker are on the radio's
  own audio codec; PulseAudio on the A33 already exposes both.
- **Codec choice: `codec2` only.** RADE V1/V2 explicitly excluded — the
  neural vocoder is too heavy for the A33.
- **Boot fast and visibly.** Banner on every boot so the operator knows
  they're on the FreeDV image and not the regular firmware.
