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
- [x] 5. ESP32-A2DP Bluetooth output, tested in isolation — hardware-confirmed,
      connected to a real BT headset and played the 440Hz test tone. No
      wiring needed, pure software/pairing. **A2DP source mode scans for
      and auto-connects to a named target SINK device -- it does not
      advertise itself to be paired with the other way around**; that was
      a real gotcha during bring-up (the device name passed to `start()`
      is the target to find, not the ESP32's own name). Per spec section
      7, wired and BT are mutually exclusive outputs; `kTestWiredPlayback`
      in `main.cpp` picks which one a given build exercises (currently
      `true` / wired, the default). **Hardware note:** classic BT
      overflowed the default partition scheme's app slot (build came in at
      1.72MB vs. ~1.25MB available) -- switched `board_build.partitions`
      to `huge_app.csv` (~3MB single app partition, no OTA) to fix it.
- [x] 6. ANO encoder + buttons — hardware-confirmed on the rebuilt board.
      Encoder rotation, all 5 buttons, and the tap/double-tap/long-press
      state machine for CENTER (spec 5.3) all correct. Board has no
      onboard pull-ups, so all 7 signal lines (encoder A/B + 5 buttons)
      need external 10k pull-ups to 3.3V. Button-to-GPIO mapping is
      hardware-order-dependent (not fixed by the board's SWn silkscreen
      labels) -- confirm/refix in `Pins.h` after any rewiring.
- [ ] 7. MAX17048 battery monitoring

Open technical risk to validate early (spec section 10): how deep
`ESP32-audioI2S`'s FLAC metadata support goes (Vorbis comments, PICTURE
block, STREAMINFO) vs. needing manual FLAC metadata-block parsing.

## UI/UX

The real UI layer (menus, Now Playing, Lyrics, Queue, Bluetooth screen,
Settings, track context menu, dark mode -- spec section 6) is implemented
in `src/ui/`, ported directly from an interactive browser simulator used
to iterate on the UX before committing it to firmware. It's wired into
`main.cpp` and drives the real TFT + ANO input, but two things are still
placeholders, called out with `TODO` comments at their definitions:

- **Library data** (`src/ui/Library.h`) is a hand-written mock
  Artist/Album/Track/Playlist set, not a real SD scan -- real FLAC
  metadata parsing is the open risk noted above (spec section 10) and
  hasn't been built yet. Selecting a mock track still plays real audio
  (`src/audio/AudioBridge.*` plays the first playable file found on the
  card), so DAC output is real; the on-screen metadata just isn't
  guaranteed to match the specific file actually playing yet.
- **Battery % and Bluetooth connection status** shown in the UI
  (`state.battery`, `state.btOn`/`state.btConnectedTo` in
  `src/state/AppState.h`) are UI-internal placeholders, not readings from
  the real MAX17048 (step 7, not yet built) or the real A2DP link
  (`src/bt/BluetoothSource.*`, still an isolated bring-up test path per
  `kTestWiredPlayback`, not merged into normal playback). Wiring both up
  is follow-up work.

This hasn't been build-tested on real hardware yet (only syntax-checked
against stub headers, no `pio run` available in this environment) --
expect the usual bring-up shakeout on first flash.

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
platformio.ini        PlatformIO project + dependency config
src/main.cpp           entry point; bring-up steps 1-6 + wires up the UI layer
src/config/Pins.h      pin assignments, cross-checked against the WROVER-B datasheet
src/state/AppState.h   full app state (mode, menu stack, now playing, queue, settings)
src/input/AnoInput.*   ANO encoder + button input logic (step 6)
src/bt/BluetoothSource.* A2DP source test tone (step 5)
src/audio/AudioBridge.* bridges UI playback intent to real ESP32-audioI2S output
src/ui/UiTypes.h        shared data shapes (Track, Menu, MenuItem, ...)
src/ui/Library.*        placeholder mock library/playlists/lyrics/BT devices
src/ui/MenuEngine.*      menu-stack construction + navigation (ported from the simulator)
src/ui/InputRouter.*     ANO events -> state transitions (ported from the simulator)
src/ui/Screens.*         TFT_eSPI rendering for every screen
src/ui/UI.*              top-level glue: boot sequence, playback clock, redraw dispatch
docs/SPEC.md            full project specification
```

Power (MAX17048, step 7) is the only bring-up step left to build.
