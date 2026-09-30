# clickpod — working notes for Claude

This file is tracked in git (not gitignored) and travels with a normal
clone. `docs/SPEC.md` and `README.md` are the source-of-truth project
docs; this file is the "how we actually got here and what to watch out
for" layer on top, kept current as work happens so a fresh session
doesn't have to re-derive it from scratch.

**Keep this file current.** Whenever you make a decision, hit a gotcha,
or finish a chunk of work, update the relevant section below in the same
turn — don't let it drift. Same discipline as `docs/SPEC.md` (amend
inline, mark amendments, never silently rewrite) and `README.md`'s status
table.

## What this project is

DIY iPod-Classic-style MP3 player firmware for an ESP32-WROVER-B (4MB
flash, 4MB PSRAM). No touchscreen — navigation is entirely an Adafruit
ANO rotary encoder + 5 directional buttons. FLAC library on SD, wired
(PCM5102A/I2S) and Bluetooth A2DP output paths, mutually exclusive.
Full spec: `docs/SPEC.md`. Repo: `github.com/ismail3005/clickpod`.

## Current status (check README.md's status table for the authoritative version)

Hardware bring-up steps 1-6 are **hardware-confirmed** on the rebuilt
physical board: PSRAM, SD, DAC/I2S playback, ILI9341 display, Bluetooth
A2DP source, ANO encoder+buttons. Step 7 (MAX17048 battery/fuel gauge) is
now **fully wired (physically) and implemented (firmware)** — `src/power/
Battery.*`, polls cell % over I2C every 2s, synced into `state.battery`
each loop from `main.cpp`. Not yet explicitly confirmed sane on-screen
(checkbox in README stays unchecked until then), but the hardware side
is done and the firmware has been flashed successfully. **Open safety
item**: the TP4056+boost module's `OUT+`/`OUT-` was at one point wired to
the 3.3V rail instead of the ESP32's 5V pin (wrong per spec 3.1, risks
overvolting the 3.3V rail) -- confirm this got fixed to 5V before trusting
a battery-powered run; currently powered via the ESP32's own USB for
flashing/dev, which is unaffected either way.

The real UI/UX layer (spec section 6) is built, flashed, and has been
through a few rounds of real-hardware fixes already (see the gotcha list
below) -- no longer "not yet hardware-tested." Current known gap: real
library scanning exists (`Library::scanFromSd()`) but Bluetooth
connection status is still a UI-internal mock, not the real A2DP link.

## How the UI got built: browser simulator first, then ported to firmware

Before writing firmware UI code, we built an interactive browser-based
simulator (a Claude Artifact) that mirrors the real ANO input timing
(`LONG_PRESS_MS=700`, `DOUBLE_TAP_MS=350`, matching `AnoInput.cpp`
exactly) and the full menu/screen state machine. This let the user
iterate on UX (button semantics, screen layouts, what long-press-Left
does, etc.) by clicking through it in a browser, far faster than
flash-test-repeat cycles on hardware. Every UX decision that came out of
that process got a `docs/SPEC.md` amendment (search for "AMENDED" /
"ADDED" markers) before being ported to firmware — the spec amendments
are the actual record of what was decided and why; this file just points
at them.

**Simulator artifact**: https://claude.ai/artifact/N15cWpVjRmUYqEyhSpJQnb
(currently at version 7). Source lives at
`/tmp/claude-0/.../scratchpad/clickpod-sim.html` in whatever session last
touched it — that path is session-specific and won't exist in a fresh
session's filesystem. **If more UX iteration is wanted before more
hardware work**: either read the artifact directly (`Artifact` tool,
`action: "read"`, the URL above) to get its current source, or — if only
firmware changes are needed and the UX is already settled — just edit
firmware directly and skip the simulator step. The simulator is a
convenience for iterating on UX *decisions*; once a decision is made and
spec-amended, both simulator and firmware should already reflect it (see
below), so there's no obligation to keep touching the simulator unless
you're actively exploring a new UX change.

**The porting discipline**: any UX change should land in *both* places
(simulator HTML + firmware `src/ui/`/`src/state/`) and get a
`docs/SPEC.md` amendment, in that order — simulator first (cheap to
iterate, user can click-test it immediately), then port the same logic
change to firmware once confirmed, then amend the spec. Don't let them
drift apart. As of this writing they're in sync (both have: slider/choice
Left-decrements, double-tap-back-to-playback, queue drag-to-reorder via
Right-tap-to-grab).

## Firmware structure (see also README.md's Layout section)

```
src/main.cpp             entry point; steps 1-7 bring-up + wires up UI::begin()/update()
src/config/Pins.h        pin assignments (see gotchas below — reconfirm after any rewire)
src/state/AppState.h/.cpp  full app state singleton (`extern AppState state;`)
src/input/AnoInput.*     ANO encoder + button tap/long-press/double-tap/isHeld (step 6)
src/bt/BluetoothSource.* isolated A2DP source test tone (step 5) — NOT merged into
                         normal playback; kTestWiredPlayback in main.cpp picks
                         wired-vs-BT test mode, they're still separate paths
src/power/Battery.*      MAX17048 fuel gauge polling over I2C (step 7) —
                         Battery::update() throttles to one poll/2s internally;
                         main.cpp's syncBatteryToUi() pushes it into state.battery
src/audio/AudioBridge.*  bridges UI "play this track" intent to real ESP32-audioI2S
                         output — plays the first real file found on SD; see
                         "known placeholders" below for why
src/ui/UiTypes.h         shared shapes: Track, Menu, MenuItem (mirrors the simulator's
                         JS object shapes 1:1 — cross-check against simulator source
                         if unsure what a field means)
src/ui/Library.*         mock Artist/Album/Track/Playlist/BT-device/lyrics data
src/ui/MenuEngine.*      menu-stack build/navigate (buildMainMenu, enterBluetooth,
                         openTrackMenu, playQueueFrom, ...) — ported function-for-
                         function from the simulator's JS of the same names
src/ui/InputRouter.*     ANO events -> state transitions (handleTap/handleLongPress/
                         handleDoubleTap/rotate/togglePower) — also ported 1:1
src/ui/Screens.*         TFT_eSPI rendering for every screen
src/ui/UI.*              boot sequence, playback clock, redraw dispatch
src/ui/Util.*            shared fmtTime(), hasAudioExtension()
```

## Real library scanning (Library::scanFromSd())

No longer mock-only. Walks the SD card root at boot (`main.cpp`, right
after `initSd()` succeeds) and replaces `Library::ALBUMS`/`PLAYLISTS`:
each top-level folder is an Artist (its subfolders are Albums, their
audio files are Tracks) UNLESS its name matches
`isPlaylistFolderName()` in `Library.cpp` (currently hardcodes just
`"funky times"`, case-insensitive) — those become ONE named Playlist
instead, walking the same Artist/Album/track structure underneath but
not contributing separate Music entries. This exists specifically
because the user's card has a `funky times/Artist/Album/track.flac`
playlist folder that duplicates albums *also* downloaded separately at
SD root — without the exclusion, Music would show every one of those
albums twice. **To add more playlist folders later, just add their
names to `isPlaylistFolderName()`.**

Track titles come from filenames (stripped extension only, no prefix
cleanup); durations are unknown (`durSec = 0`) since getting a real
duration means opening/decoding each file, not done during the bulk
scan — `UI.cpp`'s playback clock already guards against treating
`durSec == 0` as "track over" (was auto-skipping immediately before this
fix). Each scanned `Track` carries a real `path` (e.g.
`/Artist/Album/01 Song.flac`); `AudioBridge::playSomething(path)` plays
that exact file when given one, falling back to "first playable file
found" only for tracks with no known path (placeholder/mock data).

The original mock Artist/Album/Playlist data (`MOCK_ALBUMS` etc. in
`Library.cpp`) is kept as a fallback for bench-testing with no SD card
inserted, or a card scanFromSd() finds nothing on.

**Not yet built:** real FLAC metadata (tags, STREAMINFO duration) --
spec section 10 remains open; this scan is the "filename-based fallback"
approach the spec already flagged as acceptable if tag parsing turns out
to be too much. Also not built: scanning at any scale beyond what's
currently on the card has not been tested — large libraries may need a
lazier approach (load per-album on demand) instead of holding everything
in RAM at once.

## Known placeholders / intentionally-not-real-yet (flagged with TODO comments at their definitions too)
- **`state.btOn` / `state.btConnectedTo`**: driven by the UI's own mock
  `Library::BT_DEVICES` list, not the real `BluetoothSource` A2DP link.
  Those are still separate/isolated (see `kTestWiredPlayback` above).
  Merging them is real follow-up work, not yet scoped.

## Hardware gotchas worth knowing before touching wiring/pins again

- **Pin mapping is hardware-order-dependent, not silkscreen-label-
  dependent** on the ANO board — reconfirm `src/config/Pins.h` against
  reality after *any* rewiring, don't trust prior labels.
- Board has no onboard pull-ups: all 7 ANO signal lines (encoder A/B + 5
  buttons) need external 10k pull-ups to 3.3V.
- GPIO16/17 not broken out (WROVER internal PSRAM use), GPIO6-11
  reserved for internal SPI flash, GPIO12 (MTDI) is a boot-strap risk
  pin, GPIO0/2/5/15 are strapping pins (currently used safely, but be
  careful), GPIO34/35/36/39 are input-only with no internal pulls.
- `ESP32-audioI2S` is pinned to tag `3.0.12` — the default branch needs
  C++20 `std::span`, not available on this toolchain's GCC 8.4.
- `board_build.partitions = huge_app.csv` (not default) — classic BT
  pushed the build past the default partition's ~1.25MB app slot.
- A2DP **source** mode: the name passed to `BluetoothSource::begin()` is
  the target SINK to scan for and connect to (your headphones/speaker),
  **not** the ESP32's own advertised name — real gotcha hit during
  bring-up, easy to get backwards again.
- Battery/charge circuit physical wiring (confirmed): two parallel JST
  battery ports, solder-to-pad bridge to TP4056 B+/B-, B+/B- is
  bidirectional (not charge-only) — see spec section 3.1 for the full
  writeup once you're back on this.

## Build/flash reminder

```
pio run                 # build
pio run -t upload       # flash
pio device monitor      # serial monitor (115200 baud)
```

No PlatformIO CLI was available in the cloud session that built the UI
port — code was syntax/link-checked against hand-written stub headers
(Arduino.h/TFT_eSPI.h/Audio.h/SD.h) standing in for the real libraries,
which catches typos/type errors but obviously not real-hardware behavior,
timing, or actual library API mismatches (verify method names/signatures
like `audio.pauseResume()` against the real `ESP32-audioI2S` 3.0.12 API on
first build — those were written from general knowledge of the library,
not verified against its actual header).

**First real `pio run` gotcha (found, fixed):** the framework's default
C++ standard on this toolchain rejected aggregate-initializing any
struct with a default member initializer (e.g. `bool paired = false;`
on `BtDevice`, `char art = '\x01';` on `Track`/`LibraryAlbum`) — that's
relaxed aggregate init, a C++14 feature, not available in whatever
pre-C++14 mode the framework defaults to. The stub-header sandbox check
didn't catch this because it compiled with plain `g++ -std=c++17`,
which silently has the feature the real build didn't. Fixed by adding
`build_unflags = -std=gnu++11` + `build_flags: -std=gnu++17` to
`platformio.ini` (GCC 8.4 here fully supports C++17 — distinct from the
real C++20 `std::span` gap noted below for `ESP32-audioI2S`). If new
build errors show structs failing to aggregate-initialize again, check
this didn't get reverted before chasing anything else.

**First real upload gotcha (found, fixed):** `pio run -t upload` failed
right after stepping up to the configured baud rate ("Unable to verify
flash chip connection (No serial data received.)"). Classic symptom of
the actual USB-serial link (this board flashes over a separate
USB-to-serial adapter, not a devboard's onboard USB chip) not keeping up
at speed. Fixed by dropping `upload_speed` from 921600 to 115200 in
`platformio.ini`. If uploads are reliable, this can be bumped back up for
speed later — not urgent.

**First real hardware UI bug (found, fixed):** the main menu (4 root
tiles: Music/Playlists/Bluetooth/Settings) rendered as a 2x2 grid on the
actual screen instead of a vertical stack of 4 rows like the simulator.
Root cause: `Screens.cpp`'s `drawMainMenuGrid()` was written from a
mistaken assumption that the "bigger icon tiles" redesign was a 2-column
grid — it never was. Re-reading the simulator's actual CSS confirmed
`.menu-grid{flex-direction:column}`: tiles stack vertically, one per
row, each row is icon+text *side by side within itself* (that's the only
horizontal arrangement). Fixed to match. **Lesson**: when re-implementing
something from the simulator in firmware, read the simulator's actual
source (`Artifact` tool, `action:"read"`) rather than reconstructing the
layout from memory/description — memory of "bigger icon tiles" doesn't
preserve exact flex-direction.

**Known, not yet fixed:** screen refresh feels slow on button presses --
full `fillRect` + per-character SPI text draw per press, no sprite
buffering or DMA in `Screens.cpp`. Real limitation, not a bug; TFT_eSPI
supports sprite-based partial redraws, worth doing once functional gaps
are closed, not before.

**Second real hardware bug (found, fixed): full-screen flicker during
playback.** `tickPlaybackClock()` (UI.cpp) set `state.dirty = true` on
every 500ms position tick, so `drawNowPlaying()` -- which starts with a
full-body `fillRect` -- ran once a second even with no button pressed,
producing a visible flicker the whole time something played. Fixed with
a second, lighter flag: `state.progressDirty`, set instead of `dirty` on
ticks that don't change track. `Screens::render()` checks it only when
`dirty` is false and calls a new `drawNowPlayingProgress()` that redraws
just the progress-bar/time strip (a small `fillRect`, not the whole
body). `drawNowPlaying()` calls the same function for its own progress
section, so there's one source of truth for that layout. **Lesson**:
anywhere state changes on a timer/clock (not just on input), check
whether it's setting the heavy full-redraw flag when a lighter one would
do -- this class of bug won't show up in a stub-header sandbox check
since nothing there can reveal visible flicker.

**Real library scanning is live**: `Library::scanFromSd()` walks the SD
card at boot; see the dedicated section above. The user's card has a
`funky times/Artist/Album/track.flac` playlist folder that duplicates
albums also downloaded separately at SD root -- `isPlaylistFolderName()`
in `Library.cpp` hardcodes "funky times" to exclude it from Music
(it becomes its own Playlist instead). Add more names there if more
playlist folders show up.

## Working style this project has used (carry forward)

- User is terse and direct; they'll correct behavior that doesn't match
  what was actually agreed rather than what you inferred — check spec
  amendments over assumptions.
- Every genuine UX/behavior decision gets a `docs/SPEC.md` amendment
  (inline, marked, never silently rewritten) — bug fixes with no
  behavior-decision content don't need one.
- README's status table is the bring-up progress source of truth — keep
  `[x]`/`[ ]` current as steps get hardware-confirmed.
- Don't build multiple bring-up subsystems simultaneously unless the user
  explicitly asks to parallelize (as they did for UI/UX work while power
  wiring needed lab time) — the original spec's stated order still
  applies to firmware bring-up itself.
