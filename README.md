# clickpod

DIY, iPod-Classic-style MP3 player firmware, built around an ESP32-WROVER.
Navigation is entirely via an Adafruit ANO rotary encoder + directional
buttons — no touchscreen. Library is FLAC, on a standalone SD card module,
with wired (PCM5102A/I2S) and Bluetooth A2DP output paths.

Full functional/technical specification: [`docs/SPEC.md`](docs/SPEC.md).
Treat it as the source of truth for hardware, power architecture, input
mapping, and UI behavior; this README just tracks build status.

## Status

Following the bring-up order from spec section 4:

- [x] 1. ESP32 + PSRAM verification — hardware-confirmed (4194304 bytes)
- [x] 2. ESP32 + SD card init — hardware-confirmed on the rebuilt board,
      real pin assignment (18/19/23/15). Card **must be formatted FAT32**,
      not exFAT/NTFS — large SanDisk cards ship exFAT by default, which the
      Arduino `SD` library can't mount. Windows' built-in formatter caps
      FAT32 at 32GB; use Rufus (or similar) to force FAT32 on larger cards.
      **Hardware note:** the SD breakout module in use had appeared low
      quality/intermittently unreliable during earlier debugging (CRC
      errors, failed mount handshakes) independent of wiring/pins -- turned
      out fine on the rebuild. Keep an eye on it; swap the module if
      flakiness reappears.
- [x] 3. ESP32 + PCM5102A playback via `ESP32-audioI2S` (highest-risk step)
      — hardware-confirmed on the rebuilt board, real audio out of the DAC.
      `ESP32-audioI2S` is pinned to `3.0.12` (default branch needs C++20
      `std::span`, not available on this platform's GCC 8.4 toolchain).
- [x] 4. ILI9341 display alongside SD on shared SPI bus — hardware-confirmed
      on the rebuilt board. `TFT_eSPI` config is set via `platformio.ini`
      build flags rather than editing the library's `User_Setup.h`.
- [ ] 5. ESP32-A2DP Bluetooth output as a separate playback path
- [ ] 6. ANO encoder + buttons — code written (interrupt-driven quadrature
      decode, tap/double-tap/long-press state machine for CENTER per spec
      5.3), previously hardware-confirmed (button-to-GPIO mapping fixed to
      match actual physical wiring), not yet wired back in after the full
      rebuild. Board has no onboard pull-ups, so all 7 signal lines
      (encoder A/B + 5 buttons) need external 10k pull-ups to 3.3V.
- [ ] 7. MAX17048 battery monitoring

Open technical risk to validate early (spec section 10): how deep
`ESP32-audioI2S`'s FLAC metadata support goes (Vorbis comments, PICTURE
block, STREAMINFO) vs. needing manual FLAC metadata-block parsing.

## Build

This is a [PlatformIO](https://platformio.org/) project.

```
pio run                 # build
pio run -t upload       # flash
pio device monitor      # serial monitor (115200 baud)
```

Board: ESP32-WROVER-B (N4), 4MB flash / 4MB PSRAM.

## Layout

```
platformio.ini       PlatformIO project + dependency config
src/main.cpp          entry point; currently implements bring-up steps 1-4, 6
src/config/Pins.h     pin assignments, cross-checked against the WROVER-B datasheet
src/state/AppState.h  UI mode enum (MENU, NOW_PLAYING, BT_PAIRING, ...)
src/input/AnoInput.*  ANO encoder + button input logic (step 6)
docs/SPEC.md          full project specification
```

Subsystem modules (audio/, ui/, input/, bt/, power/, storage/) get added as
each bring-up step above is tackled, per the spec's stated order — don't
build multiple subsystems simultaneously.
