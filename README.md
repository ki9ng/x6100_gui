# X6100 LVGL GUI — ki9ng FreeDV-only PoC fork

This branch (`feature/freedv-poc-v0.1`) is a **proof-of-concept image** for
running [FreeDV](https://freedv.org) digital voice natively on the Xiegu
X6100. It is forked from
[gdyuldin/x6100_gui](https://github.com/gdyuldin/x6100_gui) and is intended to
be swapped in by SD card when you want to use FreeDV. Boot a normal r1cbu /
gdyuldin SD card for non-FreeDV operation.

> **Status — v0.1 (scaffold):** This image boots and identifies itself as the
> FreeDV-only PoC. It does **not yet** transmit or receive FreeDV — the
> codec2 ↔ PulseAudio audio bridge lands in v0.2. See [ROADMAP.md](ROADMAP.md)
> "FreeDV" section for the per-version plan.

This is part of an alternative firmware for X6100 using the LVGL library.

## FreeDV background

The X6100's existing L-DIG / U-DIG modes route the A33's I²S audio through
the STM32 SSB modulator transparently — codec2-encoded audio is just audio
to the STM32. No STM32 patch is required for FreeDV-on-SSB. See the
[X6100 FreeDV architecture page](https://github.com/ki9ng/x6100_gui/blob/main/docs/freedv-architecture.md)
(or the upstream project wiki) for full details.

## FreeDV-only mode

This image is intentionally narrow-scope: in v0.1 the only difference from
upstream is a startup banner and a stub FreeDV module. v0.2+ progressively
adds:

- v0.2: link `codec2` static library; expose mode select (OFF / 700D / 1600)
- v0.3: real-time encode/decode bridge between PulseAudio and the SSB modulator
- v0.4: lock the radio into L-DIG mode while FreeDV is active; UI restrictions
- v0.5: on-air interop testing with the official FreeDV GUI (PC side)

When you want normal radio operation, swap to a stock r1cbu / gdyuldin SD
card. The X6100 always prefers SD over internal flash, so the swap is just a
power-cycle.

## Installing

Open the [Releases](https://github.com/ki9ng/x6100_gui/releases) page and
download the `sdcard.*.img.zip` matching the prerelease tag (e.g.
`freedv-poc-v0.1-*`). Unzip, then with balenaEtcher or Rufus burn the
`.img` file to a microSD card. Insert into the transceiver and boot it.

## Importing ADI log

Application could mark already worked callsign in the UI.
To load information about previous QSOs - copy your ADI log to the `DATA` partition and rename it to `incoming_log.adi`.
Application will import records to own log on the next boot and will rename `incoming_log.adi` to `incoming_log.adi.bak`.

*Note*: `DATA` partition will be created after first launch transceiver with inserted SD card.


## Exporting ADI log

Application stores FT8/FT4 QSOs to the `ft_log.adi` file on the `DATA` partition of SD card. This file might be used to load QSOs to online log.


## Building

The PoC images are built by GitHub Actions on tag push (see
`.github/workflows/main.yml`). To build locally:


* Clone repositories

```
mkdir x6100
cd x6100
git clone https://github.com/gdyuldin/AetherX6100Buildroot
git clone --recurse-submodules https://github.com/ki9ng/x6100_gui
```

* Build buildroot

```
cd AetherX6100Buildroot
git submodule init
git submodule update
./br_config.sh
cd build
make
cd ../..
```

* Build app

```
cd x6100_gui
git submodule update --init --recursive
cd buildroot
./build.sh
```
