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
- [ ] 7. MAX17048 battery monitoring — wiring fully done (spec section
      3.1: battery, TP4056+boost's `OUT+`/`OUT-` correctly on the ESP32's
      5V pin -- an earlier miswiring to the 3.3V rail has been fixed --
      and the MAX17048's I2C header all connected) and firmware
      implemented and flashed (`src/power/Battery.*`, polls cell % every
      2s over I2C, SDA=GPIO21/SCL=GPIO27, wired into the UI's status bar
      via `main.cpp`'s `syncBatteryToUi()`). Checkbox stays unchecked
      until an actual on-screen reading has been confirmed sane, but the
      hardware side is done.

Open technical risk to validate early (spec section 10): how deep
`ESP32-audioI2S`'s FLAC metadata support goes (Vorbis comments, PICTURE
block, STREAMINFO) vs. needing manual FLAC metadata-block parsing.

## UI/UX

The real UI layer (menus, Now Playing, Lyrics, Queue, Bluetooth screen,
Settings, track context menu, dark mode -- spec section 6) is implemented
in `src/ui/`, ported directly from an interactive browser simulator used
to iterate on the UX before committing it to firmware. It's wired into
`main.cpp` and drives the real TFT + ANO input.

**Library data** (`src/ui/Library.*`) is no longer just a hand-written
mock set -- `Library::scanFromSd()` walks the real SD card at boot and
replaces it. Each top-level folder becomes an Artist (subfolders are
Albums, files are Tracks), except folder names recognized as playlist
folders (currently just `funky times`, see `isPlaylistFolderName()` in
`Library.cpp`), whose Artist/Album/track tree becomes one named Playlist
instead of separate Music entries -- keeps a playlist folder that
duplicates albums also downloaded separately from showing those albums
twice. Selecting a track now plays that exact file
(`src/audio/AudioBridge.*`), not just "whatever's first on the card."
Track titles start out from filenames, but real per-track metadata --
exact duration, real artist/title/album tags, embedded lyrics, and
embedded cover art -- is read directly from each FLAC file's metadata
blocks (`src/audio/FlacMeta.*`, a hand-written parser against the open
FLAC spec) the moment a track becomes Now Playing. Album art is decoded
via `src/ui/AlbumArt.*` (JPEG only). This is deliberately lazy (not done
during the bulk SD scan) to keep boot time from growing further -- see
CLAUDE.md for the full writeup, including the one known gap (scrubbing
moves the on-screen position correctly now, but doesn't yet seek the
real audio decoder to match). The mock placeholder set is kept as a
fallback for bench-testing with no SD card inserted.

**Bluetooth** is also no longer a placeholder. The Bluetooth screen is a
real on/off toggle wired to `src/bt/BluetoothSource.*`
(`MenuEngine::enterBluetooth()`), showing the real connection status
(Off / Connecting... / Connected) synced from the actual A2DP link each
loop iteration (`main.cpp`'s `syncBluetoothToUi()`). It's a single row
for one configured target device (`BluetoothSource::kTargetDeviceName`),
not a multi-device picker -- the underlying `ESP32-A2DP` source library
connects to one named sink, it doesn't enumerate discoverable devices
the way a phone's Bluetooth settings does (see spec section 8's
amendment). Turning it on still only streams a 440Hz test tone, not real
decoded audio -- routing `ESP32-audioI2S`'s output into the A2DP source
instead of the I2S DAC is separate, not-yet-done work.

Battery % (`state.battery`) is also no longer a placeholder -- see step 7
above, now synced from the real MAX17048 each loop iteration.

This has been flashed and run on real hardware through several rounds of
fixes -- see `CLAUDE.md`'s gotcha list for what's been found/fixed so
far (a menu-layout bug, a full-screen playback flicker, an O(n^2)
memory crash opening a large playlist, and boot-time SD scan tuning).

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
src/main.cpp           entry point; bring-up steps 1-7 + wires up the UI layer
src/config/Pins.h      pin assignments, cross-checked against the WROVER-B datasheet
src/state/AppState.h   full app state (mode, menu stack, now playing, queue, settings)
src/input/AnoInput.*   ANO encoder + button input logic (step 6)
src/bt/BluetoothSource.* A2DP source test tone (step 5)
src/power/Battery.*    MAX17048 fuel gauge polling over I2C (step 7)
src/audio/AudioBridge.* bridges UI playback intent to real ESP32-audioI2S output
src/ui/UiTypes.h        shared data shapes (Track, Menu, MenuItem, ...)
src/ui/Library.*        placeholder mock library/playlists/lyrics/BT devices
src/ui/MenuEngine.*      menu-stack construction + navigation (ported from the simulator)
src/ui/InputRouter.*     ANO events -> state transitions (ported from the simulator)
src/ui/Screens.*         TFT_eSPI rendering for every screen
src/ui/UI.*              top-level glue: boot sequence, playback clock, redraw dispatch
docs/SPEC.md            full project specification
```

All 7 bring-up steps are now implemented in firmware; step 7 (power) is
the only one not yet flashed/hardware-confirmed.
