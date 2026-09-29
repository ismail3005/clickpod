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
- [x] 2. ESP32 + SD card file listing over serial — hardware-confirmed
      (122GB card mounted, file listing works). Note: the card **must be
      formatted FAT32**, not exFAT/NTFS — large SanDisk cards ship exFAT by
      default, which the Arduino `SD` library can't mount. Windows' built-in
      formatter caps FAT32 at 32GB; use Rufus (or similar) to force FAT32 on
      larger cards.
- [ ] 3. ESP32 + PCM5102A playback via `ESP32-audioI2S` (highest-risk step —
      isolate before adding anything else) — code written, awaiting
      hardware test. Recursively finds the first playable audio file on the
      card and streams it over I2S; needs an actual track copied onto the
      card first (it currently only has Windows format junk on it).
- [ ] 4. ILI9341 display alongside SD on shared SPI bus
- [ ] 5. ESP32-A2DP Bluetooth output as a separate playback path
- [ ] 6. ANO encoder + buttons
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
platformio.ini      PlatformIO project + dependency config
src/main.cpp         entry point; currently implements bring-up steps 1-2
src/config/Pins.h    pin assignments (placeholders — confirm against wiring)
src/state/AppState.h UI mode enum (MENU, NOW_PLAYING, BT_PAIRING, ...)
docs/SPEC.md         full project specification
```

Subsystem modules (audio/, ui/, input/, bt/, power/, storage/) get added as
each bring-up step above is tackled, per the spec's stated order — don't
build multiple subsystems simultaneously.
