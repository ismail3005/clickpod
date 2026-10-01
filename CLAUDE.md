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
is done and the firmware has been flashed successfully. The earlier
miswiring (TP4056+boost `OUT+`/`OUT-` on the 3.3V rail instead of the
ESP32's 5V pin, risking overvolting the 3.3V rail) **has been fixed** --
confirmed by the user, no longer an open item.

The real UI/UX layer (spec section 6) is built, flashed, and has been
through a few rounds of real-hardware fixes already (see the gotcha list
below) -- no longer "not yet hardware-tested." Both remaining
placeholders are gone now: real library scanning
(`Library::scanFromSd()`) and a real Bluetooth on/off toggle wired to
the actual A2DP link (see the dedicated section below) -- BT still only
streams a test tone, not real audio, but the UI no longer shows fake
data anywhere.

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
src/bt/BluetoothSource.* real on/off-toggleable A2DP source (step 5), wired into
                         the UI's Bluetooth screen (MenuEngine::enterBluetooth())
                         — still only streams a 440Hz test tone, not real decoded
                         audio (that needs routing ESP32-audioI2S's output into
                         the A2DP callback instead of the I2S DAC — not done)
src/power/Battery.*      MAX17048 fuel gauge polling over I2C (step 7) —
                         Battery::update() throttles to one poll/2s internally;
                         main.cpp's syncBatteryToUi() pushes it into state.battery
src/net/TimeSync.*       WiFi NTP clock, no RTC hardware — background task, see
                         its dedicated section below
src/net/RadioLock.h      mutex between TimeSync (WiFi) and BluetoothSource (BT) --
                         ESP32's one shared radio, see the crash writeup below
src/state/Persist.*      NVS-backed settings + Bluetooth-on persistence across reboots,
                         plus the boot-crash guard that stops a BT-auto-resume crash
                         from becoming an infinite reboot loop (see seventh bug below)
src/audio/AudioBridge.*  bridges UI "play this track" intent to real ESP32-audioI2S
                         output — plays the given track's real path, or falls back
                         to the first playable file found for tracks with none
src/audio/FlacMeta.*     hand-written FLAC metadata-block parser (duration, tags,
                         lyrics, embedded art) — called lazily at track-start, not
                         during the bulk SD scan; see its dedicated section below
src/ui/UiTypes.h         shared shapes: Track, Menu, MenuItem (mirrors the simulator's
                         JS object shapes 1:1 — cross-check against simulator source
                         if unsure what a field means)
src/ui/Library.*         on-SD compact index (Library::ensureIndex(), /clickpod.idx) --
                         built once, read lazily/bounded per screen after that, not
                         held fully in RAM; mock data kept as a fallback -- see the
                         dedicated "on-SD compact index" section below
src/ui/MenuEngine.*      menu-stack build/navigate (buildMainMenu, enterBluetooth,
                         openTrackMenu, playQueueFrom, ...) — ported function-for-
                         function from the simulator's JS of the same names
src/ui/InputRouter.*     ANO events -> state transitions (handleTap/handleLongPress/
                         handleDoubleTap/rotate/togglePower) — also ported 1:1
src/ui/AlbumArt.*        decodes embedded FLAC cover art (via FlacMeta + TJpg_Decoder)
                         into a small cached buffer for the Now Playing screen
src/ui/Screens.*         TFT_eSPI rendering for every screen
src/ui/UI.*              boot sequence, playback clock, redraw dispatch
src/ui/Util.*            shared fmtTime(), hasAudioExtension()
scripts/patch_audioI2S.py  build-time patch for ESP32-audioI2S's FLAC maxFrameSize
                         limitation, wired in via platformio.ini's extra_scripts --
                         see the "real options for the two FLAC decode limitations"
                         section below for the full writeup
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

**Boot time**: a real 425-track playlist scan measured at ~21.7s at
20MHz SD SPI clock (confirmed clean, no errors), then bumped to 25MHz.
This is dominated by FAT directory-lookup latency (many small file
opens), not raw SPI throughput, so clock bumps alone won't cut it
dramatically. **Deliberately NOT done**: moving the scan onto a
background FreeRTOS task so the UI is interactive immediately at boot.
Considered and rejected for now -- TFT_eSPI and SD share the same
physical SPI bus, and this codebase's existing comments already note
that's only proven safe for *sequential* access from one task (see
`initDisplay()`'s comment in `main.cpp`), not true concurrent access
from two FreeRTOS tasks/cores. Getting that wrong risks trading a
deterministic ~22s boot delay for an intermittent, much-harder-to-debug
SPI corruption/hang -- not a trade worth making blind, without the
ability to test on real hardware directly. If boot speed still isn't
good enough after confirming 25MHz is reliable, the safer next step is
probably deferring the scan to first Music/Playlists access instead of
at boot (still single-threaded, no concurrency risk, just moves *when*
the same blocking work happens) -- not attempted yet either.

## Real Bluetooth toggle (BluetoothSource + MenuEngine::enterBluetooth())

No longer a placeholder device list. `src/bt/BluetoothSource.h` exposes
`begin()`/`end()`/`isConnected()`/`isRunning()`; `state.btOn`/
`btConnectedTo` are synced from the real A2DP state each loop iteration
(`main.cpp`'s `syncBluetoothToUi()`, mirrors `syncBatteryToUi()`'s
pattern). The Bluetooth screen (`MenuEngine::enterBluetooth()`) is
currently ONE row for `BluetoothSource::kTargetDeviceName` (currently
`"ULT WEAR"`) with its real status, not a device picker. Tapping that
row calls `BluetoothSource::begin()`; "Turn Bluetooth Off" calls
`BluetoothSource::end()`. The old `kTestWiredPlayback` compile-time
branch in `main.cpp` is gone -- wired output (`AudioBridge`) is always
available now, and BT is purely a runtime UI toggle on top of it.

**CORRECTED (this line was wrong)**: this section previously said
`ESP32-A2DP`'s source mode "doesn't enumerate discoverable devices" --
that's false, or at least outdated. Actually cloned and read the real
library source (`BluetoothA2DPSource.h`, `pschatzmann/ESP32-A2DP`) to
check, since the user wants a real device picker next: it genuinely
supports discovery -- `start()` with no name begins scanning instead of
connecting to a fixed target, `set_ssid_callback(bool(*)(const char*
ssid, esp_bd_addr_t address, int rssi))` fires once per discovered
device (return value presumably selects it and stops the scan -- not
yet traced into the .cpp to confirm the exact semantics), plus
`is_discovery_active()`/`cancel_discovery()` for scan state. This is
capability already present in the dependency already in `lib_deps`, not
a library swap. **Not yet built**: the picker UI itself (a device-list
screen, same row pattern as Music/Playlists), and the plumbing to get
scan results from the callback (which runs on the BT stack's own
context, not the main loop -- can't touch UI state directly from it,
needs the same stash-for-the-main-loop-to-drain pattern `AnoInput`'s
encoder ISR already uses) into something `MenuEngine` can render and let
the user select from. Also worth deciding: remember the last-picked
device and auto-reconnect on power-on, generalizing the existing
hardcoded-target auto-resume (`main.cpp`'s `BluetoothSource::begin(
BluetoothSource::kTargetDeviceName)` at boot) -- classic A2DP has no
persistent OS-level bonding the way phones do, so this is the closest
practical equivalent to "stays paired." **One more thing worth noting**:
`platformio.ini`'s `lib_deps` entry for this library
(`https://github.com/pschatzmann/ESP32-A2DP.git`) has no tag/branch
pinned, unlike `ESP32-audioI2S`'s explicit `#3.0.12` -- it floats on
whatever the default branch's HEAD is at whatever moment `pio run` last
re-resolved it, which is worth pinning to a specific tag/commit once the
picker work starts, so a future rebuild can't silently pick up an
unrelated upstream change.

**Still not real audio either way**: whichever device gets connected
(hardcoded today, picked from a list once this is built), BT only
streams a 440Hz test tone (`BluetoothSource.cpp`'s `provideTestTone()`),
not actual decoded audio -- a SEPARATE, still entirely untouched gap
from the device-picker work above. Making it stream real music means
routing `ESP32-audioI2S`'s PCM output into the A2DP source's data
callback instead of out to the I2S DAC -- a real dual-output audio
pipeline change, not done, not trivial (the two libraries currently have
no shared hook point for this). Scope it properly before attempting --
don't half-wire it.

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
- **The ILI9341 display module's backlight (BL) pin is currently wired
  directly to the 3.3V rail** -- confirmed by the user, not previously
  written down anywhere in this repo (came up mid-session, got lost when
  that context aged out -- re-confirm facts like this get written here
  the moment they're mentioned, don't rely on remembering a verbal
  mention across a long session). This means the backlight is always
  full brightness with no software control -- `state.brightness`
  (Settings slider, persisted via `Persist.*`) is purely a stored number
  right now, not wired to any real dimming; no backlight PWM pin exists
  in `Pins.h` at all. Relevant for the planned "sleep/AOD" feature (long-
  press CENTER -> keep playing + dim + show clock + ignore input) --
  real brightness control needs rewiring BL off 3.3V to a GPIO first.
  **Pin budget is tight before attempting this**: every clean GPIO is
  already allocated (see the full pin table in `Pins.h`); the only
  genuinely free pins (GPIO36/37/38) are input-only and can't drive a
  PWM output. Real options: GPIO0 (free, but the boot-mode strapping pin
  -- commonly used for a status LED/PWM output on other ESP32 projects
  after boot, but needs confirming the backlight circuit doesn't present
  enough load to interfere with the boot-mode read during reset before
  trusting it), or freeing up a currently-used pin (bigger ripple
  effect). Also unconfirmed: whether this board's BL pin is the raw LED
  line (would need a transistor/MOSFET as a low-side switch, since
  ESP32 GPIOs aren't rated to source a typical backlight LED's full
  current directly) or already buffered on the breakout board (could
  maybe drive straight from a GPIO) -- user needs to check the physical
  board before wiring this.

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

**Third real hardware bug (found, fixed): black screen on boot, looked
identical to a crash/watchdog-reboot loop (RST didn't help) -- turned
out to just be a very slow `scanFromSd()`, not actually broken.** Three
compounding causes, all fixed together:
1. `Library::scanFromSd()` ran in `setup()` *before* `UI::begin(tft)`,
   so the boot splash never drew until the (slow) scan finished --
   looked exactly like a hang/crash with zero on-screen feedback. Fixed
   by moving `UI::begin(tft)` first.
2. `SD.begin(PIN_SD_CS)` used the library's default 4MHz SPI clock and
   5-file handle limit -- fine for the original one-file bring-up test,
   painfully slow for scanning hundreds of real files, and tight given
   the playlist-folder scan nests up to 5 directories open at once
   (root->playlist->artist->album->file). Bumped to
   `SD.begin(PIN_SD_CS, SPI, 20000000, "/sd", 10)` in `main.cpp`'s
   `initSd()`.
3. No `yield()` calls anywhere in the scan's nested SD loops, real risk
   of an ESP32 task-watchdog panic/reboot on a large-enough library
   (would have looked identical to this same bug, on a loop, since the
   same slow scan re-runs every reset). Added `yield()` + periodic
   Serial progress logging in `scanArtistFolder`/`scanPlaylistFolder`
   (`Library.cpp`), and a one-off "Scanning library..." message drawn
   directly to the TFT in `main.cpp` before the scan starts, so this
   doesn't get mistaken for a hang again.

**Lesson**: a blocking operation with zero visual/serial feedback is
indistinguishable from a crash to whoever's holding the board -- always
pair a slow first-boot operation with on-screen or serial progress
output, don't wait until it's reported as "broken" to add it.

**Fourth real hardware bug (found, fixed): `abort()` crash-reboot loop
opening a real playlist -- an O(n^2) memory bug, not the redraw-speed
issue it first looked like.** User reported "click a menu item, nothing
happens, sometimes reboots" -- got the actual serial panic output
(`abort() was called`, not a hang) before guessing further, which is
what actually found this. Root cause in `MenuEngine.cpp`'s
`buildPlaylistList()`: building the per-track row menu for an opened
playlist did
```cpp
std::vector<Track> list = copy.tracks;              // full copy...
row.action = [list, idx]() { playQueueFrom(list, idx); }; // ...captured PER ROW
```
inside the loop that builds one row per track -- for the user's real
425-track "funky times" playlist, that's a fresh copy of the entire
425-track vector, once per row, ~180,000 Track copies just to open the
menu. Guaranteed heap exhaustion -> `abort()`. The exact same pattern
existed in `buildTrackList()` (album track listing: `LibraryAlbum copy =
album;` per row) -- much smaller blast radius for a normal album's track
count, but the same bug, fixed the same way. **Fix**: build the shared
data ONCE outside the loop as a `std::shared_ptr` (`make_shared<
std::vector<Track>>` / `make_shared<LibraryAlbum>`), capture the cheap
pointer per row instead of a fresh copy. `playQueueFrom`/`playAlbumFrom`
still take their list/album by value, but that copy only happens once,
at actual play time, not once per menu row at menu-*build* time.
**Lesson for any future menu-building code with a capturing per-row
lambda**: capturing a container "just in case the row needs it" is an
easy way to accidentally put an O(n) copy inside an O(n) loop -- check
whether anything being captured scales with the number of rows being
built, and share it via pointer/reference instead of copying it fresh
per row if so. Also confirmed separately: the user's card scan reported
"0 albums, 1 playlists" -- the standalone Artist/Album folders they
expected to also be at SD root (alongside `funky times/`) aren't being
found by the scan; worth checking the card's actual root layout matches
what `Library::scanFromSd()` expects (top-level folder ->
Artist/Album/track.flac) once the crash itself is confirmed fixed.

**Fifth real hardware bug (found, fixed): most menu navigation never
triggered a redraw at all.** Reported as "right-tap does nothing, then a
down-tap suddenly shows the menu" / "gotta select THEN press down for it
to register" -- looked at first like the known redraw-speed issue, but
turned out to be a correctness bug, not a performance one. `pushMenu()`
(the one function every `build*()` menu-constructing function funnels
through to enter a new menu level) never set `state.dirty`. So selecting
into Music/Playlists/an Artist/an Album/Settings correctly changed the
underlying state but never scheduled a redraw -- the screen only updated
once some LATER, unrelated action (the next UP/DOWN, which does set
dirty) happened to force one, showing the by-then-already-changed menu
and making it look like it belonged to the wrong button press. Fixed by
setting `state.dirty = true` once, in `pushMenu()` itself. Audited every
other `state.mode`/`state.menuStack` mutation site in `MenuEngine.cpp`
and `InputRouter.cpp` at the same time to confirm this was the only gap
of its kind.

## Real per-track metadata: duration, tags, lyrics, album art (FlacMeta + AlbumArt)

`Library::scanFromSd()` still only reads filenames during the bulk
directory walk (kept fast on purpose -- see the scanning section above).
Real FLAC metadata is read **lazily, once, when a track actually becomes
Now Playing** -- `MenuEngine::setNowPlaying()` calls into
`src/audio/FlacMeta.*`, a hand-written FLAC metadata-block parser (the
format itself is a fixed, openly-documented binary spec, so this is
implemented directly rather than depending on an uncertain third-party
library API):
- `readStreamInfo()` -- the STREAMINFO block (always block 0, first in
  the file) gives an exact duration without decoding anything. This
  replaces the `durSec=0` placeholder for any track that gets played,
  which was also the actual root cause of "scrubbing doesn't work":
  `InputRouter.cpp`'s scrub math (`constrain(pos, 0, durSec)`) could
  never move away from 0 when `durSec` was always 0. **The UI-side
  scrub/progress-bar system now works correctly as a result.** What's
  still NOT done: making the real audio decoder actually seek to the
  scrubbed position -- `AudioBridge` has no seek call, and
  `ESP32-audioI2S`'s seek support for FLAC specifically is genuinely
  uncertain (unlike the STREAMINFO/Vorbis-comment parsing above, this
  would depend on an unverified library API, not a fixed format spec) --
  deliberately not guessed at blind since a wrong method name there
  risks a build break for a library import, not just a missing feature.
  If real backend seeking is wanted, that's the next piece, and needs
  either the real installed header to check against or the user
  confirming the exact API on real hardware.
- `readTags()` -- the VORBIS_COMMENT block. Overrides filename-derived
  artist/title/album with the real tags when present, and picks up a
  `LYRICS`/`UNSYNCEDLYRICS` comment into `Library::LYRICS[key]` if
  present (split into lines by `MenuEngine.cpp`'s
  `splitLyricsIntoLines()`). **Known cosmetic limitation**: Vorbis
  "unsynced" lyrics are plain text with no per-line timestamps, so every
  line gets `atSec=0` -- `drawLyrics()`'s "active line" picks the LAST
  line whenever candidates tie, so real lyrics display correctly but the
  scrolling highlight doesn't track playback position the way the one
  hand-written time-synced demo entry does. Real text beats no text;
  not worth more engineering for a format that's plain text by design.
- `readPicture()` -- the PICTURE block (embedded cover art), handed to
  `src/ui/AlbumArt.*` for JPEG decode (via the new `TJpg_Decoder`
  dependency) into a small cached RGB565 buffer sized to the Now Playing
  art box. Decoded ONCE per track-start, not on every screen redraw
  (`Screens.cpp`'s `drawNowPlaying()` just blits the cached buffer) --
  re-decoding a JPEG on every redraw (volume change, play/pause, return
  from another screen, ...) would undo the whole redraw-speed effort
  below. Falls back to the pre-existing placeholder glyph square if the
  file has no PICTURE block, or the embedded art is PNG rather than
  JPEG (`TJpg_Decoder` doesn't handle PNG).

**`AlbumArt.cpp` is the least-verified piece of this round's work** --
`TJpg_Decoder`'s exact API (`setCallback`'s callback signature,
`drawJpg`'s return type/constants, `getJpgSize`) is written from general
knowledge of this specific, fairly well-known library, not checked
against its actual installed header. If art decoding doesn't compile or
doesn't render correctly, start there.

## Redraw-speed pass (partial, flagged for more if still not enough)

On top of the pushMenu correctness fix above, did one real perf pass:
`MenuEngine::moveSelection()` (UP/DOWN/rotate within a plain list --
Music/Playlists/Artist/Album/Settings/BT/track-context menus) now sets
a new, lighter `state.selectionDirty` instead of the full `state.dirty`.
`Screens.cpp`'s `updateMenuSelection()` redraws just the old + new
selected rows (a shared `drawMenuRow()` helper keeps this and the full
`drawMenu()` loop from drifting apart) instead of wiping and redrawing
the whole list on every single navigation tap -- falls back to a full
redraw automatically if the viewport needs to scroll to keep the new
selection visible, or if there's no valid previous-selection baseline
yet (e.g. right after entering a different menu).

**Deliberately NOT done this round** (flagged, not silently skipped):
- The main-menu grid (`drawMainMenuGrid()`, only 4 items) still does a
  full redraw on every selection move -- cheap enough at that size that
  it wasn't worth the same treatment yet.
- `drawQueue()` (queue list navigation) still does a full redraw too --
  same pattern as `drawMenu()`, just not done yet, same reasoning as the
  main menu (lower priority than the Music/Playlists/Artist/Album path,
  which is the one most exercised now that the library is real).
- `adjustSlider()`/`cycleChoice()` (Settings' Brightness/Sort/Appearance
  rows) still trigger a full redraw for what's really a one-row
  sub-label change.
If menus still feel slow after this, these three are the next places to
apply the same `drawMenuRow()`-style partial-redraw pattern.

## Statusbar clock (TimeSync) -- WiFi NTP, no RTC hardware

There's no RTC chip in the BOM. Instead of leaving the clock a permanent
"--:--" placeholder, `src/net/TimeSync.*` grabs wall-clock time "for
free": scans for an open (no-password) WiFi network nearby, joins it
briefly, fetches NTP time via the ESP32 core's own `configTime()`/
`getLocalTime()`, then disconnects and turns the radio off, keeping time
locally via `millis()` from then on. Re-attempts every 6h to correct
drift and to cover the case where no open network was around the first
time (a device that only works if you happen to be near an open network
will often just show "--:--" -- that's an inherent limit of "for free,
no configuration", not a bug). Settings gained a "Time zone" row
(`state.utcOffsetHours`, -12..+14, doesn't cover half-hour zones like
India UTC+5:30) since NTP gives UTC and there's no way to auto-detect
the user's zone without geolocation.

Runs entirely on a background FreeRTOS task (`xTaskCreatePinnedToCore`,
core 0) so it never blocks boot -- unlike backgrounding the SD library
scan (deliberately NOT done, see the gotcha above), this carries no
cross-peripheral risk: WiFi doesn't touch the SPI bus TFT/SD share. It
DOES share the ESP32's one radio with classic Bluetooth, so a scan+sync
attempt could cause a brief BT audio glitch if BT happens to be actively
streaming at that exact moment -- not worked around, since BT playback
is currently just a test tone anyway (see `BluetoothSource.h`); revisit
if/when BT streams real audio and this becomes noticeable.

**Higher confidence than the FlacMeta/AlbumArt work**: `WiFi.h` and
`configTime()`/`getLocalTime()` are core ESP32 Arduino framework APIs,
not a third-party library guess -- WiFi is already compiled into this
project's build regardless (visible in any `pio run` log's object file
list), so this adds no new dependency and the API surface is much
better-trodden than `TJpg_Decoder`'s.

`MenuItem` gained `sliderStep` (default 5, matching the existing
Brightness row's percentage steps) so the Time zone row can step by
whole hours instead -- `InputRouter.cpp`'s two `adjustSlider()` call
sites use `item.sliderStep` instead of a hardcoded `5`.

## Sixth real hardware bug (found, INCOMPLETE fix -- see seventh bug below): WiFi+BT coexistence crash

User turned Bluetooth on (via the UI) and got a real crash + reboot loop,
twice in a row: `[bt] Starting Bluetooth A2DP source...` followed
immediately by `assert failed: hash_map_set hash_map.c:129 (data !=
NULL)` inside Bluedroid. This appeared right after TimeSync's WiFi usage
was added in the previous round of work -- ESP32's WiFi+BT coexistence
(one shared radio) is a real, documented source of crashes when both
subsystems touch the radio around the same time, and the timing
correlation was the leading explanation at the time.

Fix attempted: `src/net/RadioLock.h`, a simple mutual-exclusion flag
between `TimeSync` (WiFi) and `BluetoothSource` (classic BT) -- whichever
is using the radio holds it for its whole active duration, the other
skips/defers its own radio use rather than risk an overlap.

**This diagnosis was WRONG, or at least incomplete** -- see the seventh
bug below. Kept in the codebase anyway since a WiFi/BT timing overlap is
still a real possible crash source in principle and the lock costs
nothing, but it demonstrably did NOT fix the actual crash the user hit.

**Separately, also logged**: a real FLAC decode failure --
`read_FLAC_Header(): FLAC maxFrameSize too large!` for a specific file
("Aerosmith - Dream On.flac"), which `ESP32-audioI2S` responded to by
closing the file and refusing to play it. This is an internal limitation
of the pinned 3.0.12 library version (some files' FLAC frame sizes
exceed whatever fixed buffer it allocates) -- not something fixable from
application code without patching the library itself, and not attempted
here. If more files hit this, it's a real constraint of this pinned
library version to flag back to the user, not a bug in this codebase.

## Seventh real hardware bug (found, best-effort fixed): BT crash was heap exhaustion, not WiFi/BT timing -- AND it bricked the whole device on every boot

After RadioLock shipped, user hit the SAME class of crash again, proving
RadioLock's diagnosis wrong: the new log showed TimeSync's WiFi cycle
fully complete and release the lock (`[time] no networks found`) BEFORE
Bluetooth even started -- they never overlapped, so a timing race can't
explain it. The assert was also different this time:
`assert failed: semphr_create_wrapper bt.c:579 (queue_buffer)`, not
`hash_map_set`. Both are heap-allocation failures deep inside Bluedroid's
own init path -- and the log right before it showed WiFi ALSO failing to
allocate its own rx buffers (`Expected to init 4 rx buffer, actual is 2`,
then `actual is 0`, then `Failed to deinit Wi-Fi driver (0x3001)`) before
BT ever got a turn. That's the real signature: the device is critically
low on free internal (non-PSRAM) heap by the time WiFi/BT try to init --
not a coexistence-timing race.

**Worse, this created a silent full-device bricking loop**: `state.btOn`
had been persisted `true` from before (the new Persist feature), so
`main.cpp`'s `setup()` auto-resumes Bluetooth on every boot -- which
crashed immediately, rebooting the device before it ever reached
`loop()`. The user's "playback stopped working for any song" report was
this: the device was never reaching a stable, interactive state at all,
not a playback regression.

Two independent fixes, both in this round:

1. **Boot-crash guard (the critical one)** -- `src/state/Persist.{h,cpp}`
   gained `markBtAttemptStarting()`/`markBtAttemptDone()`, writing a
   `btPending` NVS flag immediately before/after the auto-resume
   `BluetoothSource::begin()` call in `main.cpp`. If the device crashes
   between those two calls (i.e. BT auto-resume itself crashed),
   `btPending` survives into the next boot uncleared. `Persist::load()`
   checks this: if `btPending` is still true next boot, it means the
   previous attempt never confirmed success, so it forces
   `state.btOn = false` for this boot (and persists that immediately)
   instead of repeating the same crash forever. This is the generic
   fix regardless of whether the heap theory below is right -- it's what
   actually stops the device from bricking itself, and the same pattern
   would protect against any future BT-auto-resume crash cause too. The
   manual "Bluetooth On" UI action (`MenuEngine.cpp`'s `enterBluetooth()`
   row) was already safe from this specific loop by ordering --
   `Persist::save()` for `btOn=true` only runs AFTER `begin()` returns,
   so a crash there never persists the bad state in the first place;
   only the boot-time auto-resume path needed the explicit guard.

2. **Actual crash mitigation (best-effort, still not confirmed)** --
   `main.cpp`'s `setup()` now calls
   `esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` as the very first
   thing, before anything touches WiFi or BT. This app only ever uses
   classic BT (A2DP source) and never BLE, but the framework's default
   config reserves BLE's controller memory pool (~50KB of internal DRAM)
   regardless -- releasing it is the standard, widely-used fix for
   exactly this "WiFi/BT heap allocation failures" symptom on classic-
   BT-only ESP32 apps. `esp_bt.h`/`esp_bt_controller_mem_release()` are
   stable core ESP-IDF API bundled with the Arduino-ESP32 framework (not
   a new dependency) -- not independently verified against the real
   header in this sandboxed environment (no IDF headers available here
   to check against, unlike the WebFetch-verified `ESP32-audioI2S` API
   below), but this is a long-standing, extremely common pattern for
   this exact class of app, not a guess at an obscure/unstable surface.
   Also added: `Serial.printf` free-heap logging (`ESP.getFreeHeap()`,
   the same `EspClass` member `getPsramSize()` already used and working
   elsewhere in this file) right before the BLE release, right before
   the BT auto-resume attempt, and right before `a2dpSource.start()`
   inside `BluetoothSource::begin()` -- so if this still crashes, the
   next report has real numbers instead of needing another guess.

**If BT still crashes after this**: the boot-crash guard (fix #1) means
the device will at least stay usable and playback will work -- Bluetooth
will just stay off and need to be turned on again manually from the UI,
where a crash there is a one-off reboot, not a permanent loop. But the
underlying crash itself (fix #2) is still not confirmed fixed -- check
the new heap logs in the serial output for what the real numbers were at
each point, that's the next diagnostic step if it recurs.

## Eighth real hardware bug (found, fixed): heap exhaustion crashed TimeSync too, not just BT -- added a real pre-flight heap guard

The very next flash after the seventh-bug fix crashed again, differently:
no `[bt]`/`[time]` lines at all before `ESP_ERROR_CHECK failed: esp_err_t
0x101 (ESP_ERR_NO_MEM)` inside `ets_timer_setfn` (`esp_timer_create`),
right after the library scan finished and `Battery::begin()` logged its
(expected, non-fatal) "didn't ACK" message -- i.e. this died inside
`TimeSync::begin()`'s background task, before it even got to log "no
networks found", while trying to bring WiFi up for the first time. This
confirms the seventh bug's diagnosis was on the right track (heap
exhaustion right after the ~425-track scan) but incomplete -- it's not
BT-specific, WiFi hits the exact same wall, and neither subsystem can be
trusted to fail gracefully on its own: both abort the WHOLE device
instead of just failing to start, because IDF's `ESP_ERROR_CHECK` calls
inside WiFi/BT init are unconditional aborts, not something app code can
catch or recover from after the fact.

Since both subsystems fail the same way for the same underlying reason,
the real fix has to happen BEFORE either one is touched, not per-
subsystem after the fact: `src/net/RadioLock.h` gained
`radioHeapOk(who)`, checking `heap_caps_get_free_size(MALLOC_CAP_
INTERNAL)` -- specifically INTERNAL (non-PSRAM) heap, since that's the
pool WiFi/BT's DMA-capable buffers actually come from, and the earlier
`ESP.getFreeHeap()` logging (158848 bytes reported right after the BLE
mem-release call, near the very start of boot) was likely misleading
precisely because it can include PSRAM headroom that WiFi/BT can't
actually use -- against a conservative placeholder threshold
(`kMinInternalHeapForRadio`, 60KB, NOT backed by documented IDF minimums,
just a starting guess to tune from real logged numbers). Both
`TimeSync::tryOnce()` and `BluetoothSource::begin()` now call this FIRST,
before anything else, and skip (log + return) rather than proceed into
what's demonstrated twice now to be an unrecoverable abort(). `TimeSync`
also gained a fast retry (`kSkippedRetryDelayMs`, 2 minutes) specifically
for attempts skipped this way -- distinct from the normal 6h resync
interval, which still applies when an attempt actually ran and failed
for an ordinary reason (no open network in range, couldn't join, NTP
didn't answer) -- so a transient low-heap moment at boot doesn't mean
waiting 6 hours for the clock to ever sync.

Also added: `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` logging
alongside every existing `ESP.getFreeHeap()` log point in `main.cpp`,
plus a new one right after the library scan finishes (the scan is the
obvious concurrent consumer of internal heap right before both crashes
hit) -- so if this recurs, the real internal-vs-total split is visible
instead of needing another blind guess.

**Still not fully closed out**: the `kMinInternalHeapForRadio` threshold
is a placeholder, not a verified minimum -- next real boot's serial log
(now with internal-heap numbers at every stage) is what tells us whether
60KB is comfortably enough, uncomfortably tight, or needs raising. If
WiFi/BT still abort even above this threshold, the number needs to go up,
not the approach rethought. If they're consistently skipped for lack of
headroom, the real structural fix (not attempted yet, flagged as a
bigger change) would be moving the library's per-track String data
(title/artist/album/path -- `Library::scanFromSd()`'s Track structs) into
PSRAM-backed storage instead of the default internal-RAM allocator, since
425 tracks x 4 String fields each is a plausible real chunk of the
internal heap that's sitting unused in PSRAM instead.

## Real seek + real position sync (verified API, not guessed)

Unlike `TJpg_Decoder` (guessed from general knowledge, flagged as the
least-verified piece of that work), `ESP32-audioI2S` 3.0.12's actual
header was fetched and checked (`WebFetch` against the real
`github.com/schreibfaul1/ESP32-audioI2S` tag) before writing this --
confirmed real methods: `setAudioPlayPosition(uint16_t sec)`,
`getAudioCurrentTime()`, `getAudioFileDuration()`. `AudioBridge` now
exposes `seekTo()`/`currentTimeSec()` wrapping these.

This fixes "scrubbing looks like it works but doesn't actually move the
audio" -- `InputRouter.cpp`'s `rotate()` now calls
`AudioBridge::seekTo()` in addition to updating the UI's own
`posSec`. It also replaces the old always-simulated position clock:
`UI.cpp`'s `tickPlaybackClock()` now syncs `state.now.posSec` from
`AudioBridge::currentTimeSec()` (the REAL decoder position) whenever
playing a real file, falling back to the simulated increment only for
placeholder/mock tracks with no real path. The progress bar and Lyrics
screen were both silently working off a locally-guessed number before
this that had no relationship to what was actually playing.

## Approximate lyrics sync

`LYRICS`/`UNSYNCEDLYRICS` Vorbis comments are plain text with no
per-line timestamps (the format's own name says "unsynced"). Previously
every line got `atSec=0`, so `drawLyrics()`'s "active line" picker
always landed on the last line the whole song, which read as "lyrics
don't move." `MenuEngine.cpp`'s `splitLyricsIntoLines()` now spreads
lines evenly across the track's real duration (`line i` at
`i/(n-1) * durSec`) instead -- not real sync (no real per-line timing
data exists to use), but the highlight now visibly advances over the
course of the song, which is what "lyrics should move with the song"
actually needs even without being frame-accurate. Real sync would need
an LRC-style timestamped format, which FLAC Vorbis comments don't carry.

## On-SD compact index, replacing the old "hold the whole library in RAM forever" model

The old `Library::scanFromSd()` did TWO expensive things every single
boot: (1) a slow FAT walk of the whole SD card (~22s for the user's real
425-track playlist), and (2) permanently held every track's artist/
album/title/path as separate heap-allocated `String`s in RAM for the
entire session (~95KB of internal RAM for that one playlist -- see the
eighth hardware bug above, this is what made BT/WiFi unable to get enough
internal heap headroom to start at all). Neither scales: the user's
planning on loading hundreds more tracks onto a much bigger SD card, and
"hold everything, always, in RAM" gets worse linearly with library size
regardless of whether BT is involved.

Replaced with a compact on-SD index file, closer to how real portable
players (e.g. the iPod's iTunesDB) actually do this -- `Library::
ensureIndex()`/`Library.cpp`:

- **The slow FAT walk only ever happens once.** `ensureIndex()` checks
  whether `/clickpod.idx` already exists on the card; if so, it's trusted
  as-is and used directly -- no re-walking SD, no auto-detection of card
  content changes (that would mean doing the expensive walk anyway,
  defeating the point). Only the FIRST ever boot (or a manual "Rescan
  library" row added to Settings, which calls `ensureIndex(true)`) does
  the real directory walk; every boot after that just checks the file
  exists and moves on. This is also the fix for the ~22s boot-time
  complaint from earlier in the session, as a side effect -- most boots
  now skip that entirely.
- **Nothing holds the whole library in RAM anymore.** The index build
  itself writes records directly to the SD file as it walks (never
  accumulates a big in-RAM vector, even during the one-time build).
  Reading it back is lazy and bounded: `indexArtists()`/
  `indexAlbumsForArtist()`/`indexPlaylists()` do a fast SEQUENTIAL read of
  the compact index file collecting just names+counts (cheap, safe to
  call on every menu open), and `indexTracksForAlbum()`/
  `indexTracksForPlaylist()` materialize a `vector<Track>` for ONLY the
  one album or playlist actually being opened -- not the whole library.
  `MenuEngine.cpp`'s `buildTrackListFromIndex()`/
  `buildPlaylistTrackListFromIndex()` hold that vector in a `shared_ptr`
  scoped to the menu screen it's for (same pattern as the fourth hardware
  bug's fix), so it's freed again once the user navigates away, and
  re-read from the index (fast sequential file read, not a FAT walk) next
  time. RAM usage is now bounded by "the biggest single album/playlist
  currently open," not "everything on the card."

**Index file format** (`/clickpod.idx`, SD root): 4-byte magic `"CPX1"`
(doubles as a version tag) + 4-byte LE track count (informational only --
readers rely on EOF to know when to stop, not this value, so a
wrong/stale count can't cause a bad read) + that many variable-length
records: 1-byte kind (0=Music track, 1=Playlist track), then
length-prefixed string fields (artist/album/title/path, plus a playlist
name for kind 1). Lengths are `uint16` (not `uint8`) specifically because
a full nested SD path (playlist/artist/album/filename) can plausibly
exceed 255 bytes with real long filenames even though no single name
component usually would -- cheap insurance (1 extra byte per field)
against silent truncation.

**"Add to Playlist" under the new model**: playlist edits from the UI
(`openTrackMenu`'s "Add to Playlist" row) are appended to a small in-RAM,
session-only overlay (`Library::addToPlaylist()`/`extraPlaylistTracks`),
never written back to the index file on SD. This matches the OLD
behavior exactly -- direct mutation of the in-RAM `PLAYLISTS` vector also
never persisted across a reboot -- so it's not a regression, just the
same ephemeral behavior under the new storage model. `indexPlaylists()`/
`indexTracksForPlaylist()` merge these overlay entries in when reporting
counts/tracks for a playlist.

**Mock/placeholder fallback is untouched**: `Library::ALBUMS`/
`PLAYLISTS` (the old hand-written mock data) still exist exactly as
before, for bench-testing with no SD card or a card `ensureIndex()` finds
nothing playable on. `Library::usingIndex()` is the flag `MenuEngine.cpp`
checks everywhere to pick between the new `index*()` functions and the
old direct-`ALBUMS`/`PLAYLISTS`-iteration path (`buildArtistList`/
`buildAlbumList`/`buildPlaylistList`/the "Add to Playlist" submenu all
branch on this) -- zero behavior change on the mock path, only the real-
SD path changed.

**Not done, flagged as the natural next step if a library gets into the
thousands of tracks**: this is bounded-per-open, not fully paginated --
opening a single playlist/album with, say, 5,000 tracks in it would still
materialize all 5,000 as `MenuItem` rows at once (just not held forever
afterward). True virtual-scrolling (only ever materializing the rows
currently on screen, re-querying the index as you scroll) would need
real restructuring of `MenuEngine`'s `build*()` functions and `Screens`'
menu-rendering/scroll logic, and wasn't attempted here -- the current
design was sized to "hundreds more tracks," which this comfortably
covers, not an open-ended multi-thousand-track collection.

**Verification**: `Library.cpp`'s full new implementation and
`MenuEngine.cpp`'s changed functions were both compiled clean (zero
errors) against hand-written stub headers in this sandbox, same
discipline as the rest of this project -- no real `pio run` available
here. `Screens.cpp`'s one addition (`showBusyMessage()`) was not
separately compiled (its own stub dependency chain is large) but uses
only `tftPtr`-based calls already proven working elsewhere in that exact
file, so this is lower-risk than an unverified change would be.

## Ninth real hardware bug (found, fixed): playback silently stalled forever on a real decode failure, no recovery

User hit the known `FLAC maxFrameSize too large!` limitation again (a
different file this time, "TOOL - Schism.flac") and reported "playback
dont work" -- the underlying decode failure itself is the same pinned-
library limitation documented earlier (not fixable from here), but the
REAL bug this exposed is that nothing in the UI ever noticed the file
failed to play. `ESP32-audioI2S` just logs the error and closes the file
internally -- there's no exception or failure callback app code gets, so
`state.now.playing` stayed `true` and the UI just sat there showing
"playing" a track that was never actually producing sound, forever,
until the user manually skipped.

Fixed generically (not by string-matching this one error message, which
would miss any OTHER decode failure mode): `AudioBridge::isRunning()`
(new) wraps `Audio::isRunning()`. `NowPlaying` gained `startedAtMs`/
`playbackConfirmed` (`AppState.h`) -- set when a real track starts
(`MenuEngine::setNowPlaying()`). `UI.cpp`'s `tickPlaybackClock()` checks
`AudioBridge::isRunning()` each tick for a track that hasn't confirmed
playback yet; if it's still not running after a grace period
(`kPlaybackStartGraceMs`, 3000ms -- a sized-generously guess, not
hardware-measured, tune if real files slow-start past this or a failure
takes visibly longer than this to get skipped), it's treated as a
decode failure and `MenuEngine::playNextInQueue()` is called
automatically, same as reaching the end of a track. `Audio::isRunning()`
itself is a real, long-standing ESP32-audioI2S method, but wasn't
independently header-verified this round the way `setAudioPlayPosition`/
`getAudioCurrentTime` were earlier (WebFetch against the real header) --
lower risk than a guess at an obscure API, since `isRunning()` is one of
this library's most commonly used/documented calls, but flagging the
difference in confidence level honestly.

## Row icons added to every list screen (previously only Bluetooth had one)

`MenuItem::icon` was only ever being SET for the 4 main-menu tiles
(`buildMainMenu()`) -- every other screen (Music's artist/album/track
lists, Playlists, Settings, the "Add to Playlist" submenu) left it empty,
and `drawMenuRow()` (the renderer every non-main-menu list screen shares)
never drew an icon at all even when one was set. The Bluetooth screen's
apparent icon was actually a special case in `drawMenu()`'s title-bar
code (`if (state.mode == AppMode::BT) drawBtGlyph(...)`), not a per-row
icon -- hence "only the BT one has an icon."

Fixed by actually rendering `MenuItem::icon` in `drawMenuRow()`
(`Screens.cpp`'s new `rowIconFor()`/`drawRowIconIfAny()`) -- a small
colored rounded-rect + single capital letter badge, reusing the exact
same visual language `drawMainMenuGrid()`'s tile badges already use
(proven working there), rather than hand-drawing new vector glyphs per
icon type blind the way `drawBtGlyph()` does -- much lower risk of a
rendering bug with no way to see it before the user flashes it.
`MenuEngine.cpp`'s `build*()` functions now set `.icon` per row type:
`"artist"`/`"album"`/`"track"`/`"playlist"` for Music/Playlists rows
(both the real index-backed path and the mock fallback path),
`"brightness"`/`"sort"`/`"theme"`/`"timezone"`/`"rescan"` for Settings'
non-Bluetooth rows, `"bt"` for both Bluetooth-screen rows (reuses the
real `drawBtGlyph()` vector shape instead of a letter, same as before).

**Not cross-checked against the browser simulator this round** -- this
is new firmware-only visual polish (a rendering gap, not a ported
behavior/UX decision), and the user asked to move fast on a cluster of
bug fixes rather than a UX iteration round. If the simulator should show
matching row icons too, that's a follow-up, not done here -- see
CLAUDE.md's usual porting-discipline note.

## Finer Now Playing scrubbing

`InputRouter.cpp`'s `rotate()` scrub step in `NOW_PLAYING` mode dropped
from 3 seconds/encoder-tick to 1 -- user feedback that it felt too
coarse for precise scrubbing. Simple fixed-step change, not a switch to
variable/accelerating scroll speed (e.g. some iPod-style wheels speed up
the longer you keep turning) -- if 1s/tick now feels too SLOW to cross a
long track, that's the next thing to try, not attempted here. Menu list
navigation (UP/DOWN/rotate moving the selected row) was checked and is
already exactly 1 row per physical encoder detent (`AnoInput.cpp`'s
quadrature decode uses a standard full-step transition table) -- nothing
found there to make "finer," that's already as granular as a discrete
list gets.

## "Lyrics sometimes not available" -- likely not a bug

Investigated `FlacMeta.cpp`'s tag parsing for a missed-lyrics bug and
didn't find one: `parseVorbisComment()` already does `key.toUpperCase()`
before comparing against `"LYRICS"`/`"UNSYNCEDLYRICS"`, so case
variations in how a tagger writes the field name are already handled.
The much more likely explanation is simply that some files in the
library genuinely don't have either of those two Vorbis comment fields
set at all -- not every FLAC tagging tool writes lyrics, and some use
other/nonstandard field names this doesn't look for. If a specific file
is known to have lyrics embedded under a different tag name, that name
can be added to the `key ==` checks in `parseVorbisComment()` -- not
done blind without knowing what name to add.

## Tenth real hardware bug (found, fixed): second FLAC limitation -- 24-bit samples unsupported, now detected and skipped immediately

The auto-skip fix (ninth bug, above) is working as intended -- user
confirmed "some of them it works" after a failure, meaning the generic
isRunning()-based detection correctly caught and skipped a failing file.
The new file that triggered it exposed a SEPARATE, distinct
`ESP32-audioI2S` 3.0.12 limitation from the earlier `maxFrameSize too
large` one: `read_FLAC_Header(): bits per sample must be 8 or 16, is 24`
-- a hard requirement baked into the library itself (explicit in its own
error text), not a bug in this codebase. 24-bit FLAC ("hi-res" rips) is
just not decodable by this pinned library version at all; the only real
workaround is re-encoding affected files to 16-bit FLAC before copying
them to the card (a practical, lossless-for-portable-listening step the
user would do outside this codebase, not something fixable here).

Since STREAMINFO already gets opened/read for every real track anyway
(`MenuEngine::setNowPlaying()`'s existing duration lookup), this is now
detected proactively instead of waiting for the generic 3-second grace-
period failure: `FlacMeta::StreamInfo` gained `bitsPerSample` (parsed
from the same packed bit-field STREAMINFO already decodes duration from
-- see the expanded bit-layout comment in `FlacMeta.cpp`). If it's
anything other than 8 or 16, `setNowPlaying()` skips calling
`AudioBridge::playSomething()` entirely (no point opening/partially-
decoding a file already known to fail) and logs a specific, useful
message naming the real reason instead of the generic "never started
playing" one. To reuse the already-tested, non-recursive skip mechanism
in `UI.cpp`'s `tickPlaybackClock()` rather than adding a second,
parallel skip path, it backdates `NowPlaying::startedAtMs` by exactly
`UI::kPlaybackStartGraceMs` (moved from `UI.cpp`-local to public in
`UI.h` so `MenuEngine.cpp` can reference the same constant) -- the very
next tick sees the grace period already elapsed and skips it through the
same code path as a generic decode failure, just effectively instantly
instead of after a 3-second wait.

**If more FLAC limitations of this pinned library version turn up**
(this makes two: frame size, now bit depth), the same pattern applies --
check what STREAMINFO/the error message reveals, see if it's cheaply
detectable up front from data already being read, and skip proactively
with a specific message rather than relying solely on the generic
grace-period fallback every time.

## Eleventh real hardware bug (found, fixed): Lyrics screen never actually redrew during playback

User reported "lyrics dont track" even after the approximate-sync fix
above. Root cause: `state.now.posSec` WAS updating correctly every tick
(the real-seek-sync fix from earlier), but the Lyrics screen itself was
never being repainted to show it. `UI.cpp`'s `tickPlaybackClock()` only
ever set the lighter `state.progressDirty` flag on an ordinary position
tick, and `Screens::render()` only acts on `progressDirty` for
`AppMode::NOW_PLAYING` (that's literally what it was built for -- the
Now Playing progress bar). Sitting on the Lyrics screen, nothing ever
set `state.dirty`, so `drawLyrics()` only ran on the rare actual mode/
selection change that happened to also touch `dirty` -- the active-line
highlight was frozen from the moment you opened Lyrics until you left
and came back.

Fixed in `tickPlaybackClock()`: when `state.mode == AppMode::LYRICS`,
use the heavier `state.dirty` (full redraw) instead of `progressDirty`
on a position tick. Not the same lighter partial-redraw treatment Now
Playing's progress bar got -- every visible lyric line's Y position
shifts together whenever the active line changes (it's a centered
scrolling view, not an independent strip), so a true partial redraw
isn't as simple here. Real tradeoff (a full-body redraw every ~500ms
while sitting on this specific screen), flagged rather than silently
accepted -- lower-impact than Now Playing's flicker was since it's a
screen people dip into, not sit on for most of playback. Candidate for
the same partial-redraw treatment later if it's noticeable in practice.

## Lyrics timestamp stripped from on-screen text, real LRC sync used when present

Separately, the user reported a literal timestamp showing up "in front
of the lyrics" on screen. Cause: some taggers/rippers write LYRICS/
UNSYNCEDLYRICS Vorbis comments in LRC format (`[00:08]Some line`) even
though the field name implies plain unsynced text -- `splitLyricsIntoLines()`
was just splitting on newlines with no awareness of that markup, so it
rendered verbatim, tag and all.

Fixed with a proper fix, not just a strip: `MenuEngine.cpp` gained
`stripLeadingTimestamp()`, which parses and removes a leading
`[mm:ss]`/`[mm:ss.xx]`/`[hh:mm:ss.xx]` tag (handles more than one on the
same line, which LRC allows) and returns the parsed seconds. A genuine
bracketed section marker with no colon inside (e.g. `[Chorus]`, or the
hand-written mock demo lyric's `[instrumental intro]`) is left alone --
only things that actually parse as a clock get stripped, so this can't
eat real lyric text that happens to start with a bracket.
`splitLyricsIntoLines()` now checks whether the clear majority (>=75%,
tolerating one bare/untagged line) of a file's lines carried a real
parsed timestamp: if so, it uses those REAL seconds directly instead of
the even-spread approximation -- genuine per-line sync for any file
whose tag actually has it, not just the visual fix of removing the
visible tag text. Falls back to the existing even-spread approximation
exactly as before for files with genuinely plain, untimed text.

## Lyrics screen flicker (found immediately after the last fix, fixed properly)

The previous round's "Lyrics don't track" fix (full `state.dirty` redraw
on every position tick while on that screen) worked but introduced a
visible flicker -- the exact same class of bug as the second hardware
bug (Now Playing's progress-bar flicker), just reintroduced on a
different screen. Fixed properly: `MenuEngine::activeLyricIndex()` (new,
public -- the same active-line computation `drawLyrics()` already did
inline, now shared instead of duplicated) lets `UI.cpp`'s
`tickPlaybackClock()` compare the current active line against the last
one it saw (`lastLyricsActiveIdx`) and only actually set `state.dirty`
when it changed. Real lyrics only change lines every several seconds,
not every 500ms tick, so this cuts the redraw rate dramatically while
keeping the screen genuinely tracking. Still a full-body redraw when it
DOES fire (every visible line's Y shifts together in this centered
scrolling view -- a true partial/row-level redraw isn't as
straightforward as Now Playing's single progress-bar strip was), just
no longer firing on every tick regardless of whether anything changed.

## PSRAM pushed harder -- user's explicit call ("don't hesitate to use the sram... i paid extra for on this wrover")

Three concrete moves, on top of everything the radio-heap-guard work
above already established about internal RAM being the genuinely scarce
resource on this board (PSRAM: 4MB, essentially unused before this):

1. **`AlbumArt.cpp`'s cached art buffer** (`artBuf`, ~17KB:
   `kSize*kSize*sizeof(uint16_t)`, allocated once in `begin()` and held
   for the entire session) -- `ps_malloc()` instead of `malloc()`. Pure
   pixel data (TJpg_Decoder's callback writes it, `TFT_eSPI::pushImage()`
   just reads it back over plain SPI -- no DMA requirement on the source
   buffer), so there was no reason this was ever in internal RAM.
2. **`FlacMeta.cpp`'s `readPicture()` read buffer** (the raw embedded
   JPEG bytes, up to 2MB capped, typically tens-to-hundreds of KB for a
   real cover image) -- `ps_malloc()` instead of `malloc()`. Transient
   (freed right after `AlbumArt::loadForTrack()` decodes it), but a
   transient spike that size in internal RAM, layered on top of whatever
   else is active during playback, is exactly the kind of thing that's
   been pushing internal heap dangerously low (user's own log: 6712
   bytes free during active playback, right when `TimeSync` correctly
   declined to sync rather than risk the crash the heap guard exists to
   prevent -- that decline was the guard working as intended, not a bug).
3. **`main.cpp`'s `setup()`**: `heap_caps_malloc_extmem_enable(4096)`,
   right after `verifyPsram()`. A real, long-standing Arduino-ESP32 core
   function -- any plain, capability-unspecified `malloc()`/`new` of 4KB
   or more now prefers PSRAM automatically, as a systemic complement to
   hunting down every individual large-allocation call site by hand (a
   `std::vector<Track>` growing for a big opened playlist/album, a large
   lyrics text buffer, anything else not explicitly handled above).
   **Why this is safe for WiFi/BT/I2S's own DMA-capable buffers
   specifically**: the threshold only affects calls that don't specify a
   capability. Code that explicitly requests `MALLOC_CAP_DMA`/
   `MALLOC_CAP_INTERNAL` (which any well-behaved driver needing DMA-safe
   memory does) bypasses this threshold entirely and still gets internal
   RAM regardless. **Honest residual caveat**: that's the documented
   contract of ESP-IDF's capability-tag allocator, not something
   independently verified against `ESP32-A2DP`/`ESP32-audioI2S`'s actual
   internal allocation calls in this sandbox (no IDF/library source
   available here to check). The existing heap guards (`RadioLock.h`)
   already protect against WiFi/BT failing to START under low memory --
   if audio or Bluetooth output instead gets audibly glitchy/corrupted
   (a different symptom, not just a refusal-to-start) after this change,
   that's the first thing to suspect, and this one call is what to revert.

## Open question, not attempted: real options for the two FLAC decode limitations (24-bit, maxFrameSize) without re-encoding

User's real music collection has files hitting both known
`ESP32-audioI2S` 3.0.12 limitations (24-bit samples unsupported;
`maxFrameSize too large` on some files). The firmware-side fix so far is
just "detect and skip fast, don't stall" -- it doesn't make those files
actually playable, and re-encoding the whole affected set is a real
hassle the user explicitly doesn't want to repeat. Options, not
attempted, for whenever this gets picked back up -- roughly effort/risk
ascending:

1. **Re-encode just the affected files, not re-download them.** If the
   user still has the original files anywhere off the SD card (a
   computer, wherever they were ripped/downloaded from originally), only
   THOSE specific files need converting to 16-bit FLAC (`ffmpeg -i in.flac
   -sample_fmt s16 out.flac`, or any FLAC-capable tool), then copied back
   onto the card in place and a "Rescan library" run. Lowest effort,
   zero firmware risk, but does need access to source files outside the
   SD card -- if the SD card is the only copy left, this still means
   pulling files off it, converting, and putting them back, not
   literally re-downloading from the original source.
2. **Log which files failed, directly on the SD card, not just serial.**
   Not a fix for the limitation itself, but makes option 1 far less
   painful to act on -- right now the user has to catch the skip message
   live in the serial monitor to know which files are affected. A small
   addition to the skip path (`UI.cpp`'s `tickPlaybackClock()` /
   `MenuEngine.cpp`'s `setNowPlaying()`) could append failed paths +
   reasons to a plain text file on the card (e.g. `/clickpod_failed.txt`),
   so the user can just open that file to get an exact list of what
   needs re-encoding. Low effort, low risk, doesn't touch playback itself.
3. **DONE (build-time patch, not a fork) -- `ESP32-audioI2S`'s FLAC frame
   buffer size.** Investigated by actually cloning the real pinned 3.0.12
   tag and reading `Audio.cpp`'s FLAC decode path directly (not the public
   header this project usually checks against -- this needed the real
   implementation). Confirmed: `read_FLAC_Header()` reads the file's real
   max frame size from its own STREAMINFO block (`m_flacMaxFrameSize`,
   declared `uint16_t` in `Audio.h` -- can never exceed 65535) and rejects
   the file if it exceeds the decoder's current input-buffer threshold
   (`InBuff.getMaxBlockSize()`, bumped from a generic 1600-byte MP3/AAC
   default to 16384 for FLAC specifically in `initializeDecoder()` -- but
   16384 still isn't always enough; the user's "TOOL - Schism.flac" needed
   18989). The library already grows this same buffer for every other
   codec via `InBuff.changeMaxBlockSize()`, and even has a commented-out
   call to do exactly this sitting right in `read_FLAC_Header()` --
   `//        InBuff.changeMaxBlockSize(m_flacMaxFrameSize);` -- just
   placed AFTER the function's early `return -1`, so original code could
   never actually reach it. The real backing buffer is PSRAM and hundreds
   of KB (`inputBufferSize: 638965 bytes` in the user's own serial log) --
   `m_maxBlockSize` is just a threshold the decode loop checks against,
   not a hard memory ceiling, so growing it to fit one specific file's
   real (and type-bounded, so inherently safe) frame size is safe.

   Couldn't fork the library under the user's GitHub account to apply this
   properly (session's GitHub access is scoped to `ismail3005/clickpod`
   only; both `mcp__github__fork_repository` and `add_repo` with push
   access to the external repo were refused -- forking/widening repo
   access needs the user's own explicit action, not something to grant
   from inside a coding session). Vendoring the whole ~10K-line library
   into this repo to change a few lines was also ruled out -- that means
   hand-maintaining a permanent fork instead of tracking the clean
   upstream tag.

   Went with a **build-time patch script** instead --
   `scripts/patch_audioI2S.py`, wired in via `platformio.ini`'s new
   `extra_scripts = pre:scripts/patch_audioI2S.py`. Runs before every
   compile: finds the downloaded library's `Audio.cpp` under
   `$PROJECT_LIBDEPS_DIR`, and if the original (exact-string-matched, not
   guessed) too-large check is present and unpatched, replaces it with a
   version that calls `InBuff.changeMaxBlockSize(m_flacMaxFrameSize)` to
   grow the buffer to fit instead of refusing the file -- only when
   `m_flacMaxFrameSize` is nonzero and the unpatched fallback (refuse +
   log, same as before) still applies otherwise. Idempotent (a marker
   comment makes a second run a safe no-op) and fails safe if the
   library's source ever doesn't match what's expected (logs a warning,
   leaves the file untouched, rather than corrupting it blind). The exact
   find-and-replace was tested against a real clone of the pinned 3.0.12
   tag in this sandbox (confirmed the original text matches byte-for-byte
   and the patched result is syntactically valid), but the actual BUILD
   (does PlatformIO's `extra_scripts` hook fire as expected, does the
   patched code compile and behave correctly on real hardware) is **NOT
   verified** -- no `pio run` available here, same standing caveat as
   everything else in this project done this way. First real build after
   this is what confirms it.

   Does NOT help the 24-bit-samples limitation -- separate, intentional
   hard requirement in the same library (`bps != 8 && bps != 16`), not a
   buffer-size issue this patch touches.
4. **Patch in real 24-bit support.** The hardest, highest-risk option --
   modifying the decoder's internal PCM handling to accept and downmix/
   truncate 24-bit samples to 16-bit (or pass them through if the I2S
   output path can take it) touches the core of a complex audio decoder
   this session hasn't read the internals of. Real risk of subtle audio
   corruption/distortion bugs that are hard to verify without hardware
   access and deep familiarity with the library. Not recommended as a
   first move -- try 1-3 first.
5. **Upgrade the pinned toolchain/library version.** The real,
   structural fix would be moving off `ESP32-audioI2S` 3.0.12 to a
   version with better FLAC support -- blocked by the documented C++20
   `std::span` / GCC 8.4 incompatibility (see the hardware-gotchas
   section). Upgrading the ESP32 Arduino platform/toolchain itself is a
   significant, project-wide undertaking (re-verify everything still
   builds, possible new incompatibilities elsewhere) -- biggest lift of
   all these options, only worth it if 1-4 turn out insufficient.

**Status**: option 3 is done (see above) -- pending real-hardware
confirmation on the next flash. If it works, that should fix every
maxFrameSize failure without touching a single file. What's left after
that flash: the 24-bit limitation still needs option 1 (re-encode) if the
user wants those specific files playable -- option 3 doesn't touch it.
Option 2 (failed-files log on the SD card) is still worth doing
regardless, to make finding which files are 24-bit easy. 4/5 stay
lowest-priority, only relevant if 24-bit support itself is ever wanted
without re-encoding.

## Settings + Bluetooth-on persistence (Persist / NVS)

`src/state/Persist.*` saves brightness, dark mode, sort preference, time
zone, and whether Bluetooth was left on to the ESP32's NVS flash (via
the core `Preferences` library -- built into the framework, not a new
dependency) and restores them at boot, before `UI::begin()` so the first
screen already reflects saved settings. Each setting's change site
(`MenuEngine.cpp`'s `buildSettings()` setters, the Bluetooth on/off
actions) calls `Persist::save()` directly -- no debouncing, settings
change rarely enough that this is simpler and safe for NVS wear.

If `state.btOn` was persisted `true`, `main.cpp`'s `setup()` calls
`BluetoothSource::begin()` at boot to resume it -- this is what "want
paired BT stuff to persist" currently means, since there's only ever one
configured target device (`BluetoothSource::kTargetDeviceName`) to
resume, not a list of paired devices to choose from (see the spec 8
amendment on why). If the UI ever supports configuring a different
target device name, that choice belongs in `Persist` too.

## Next session plan (as of 2026-10-01, agreed in a planning-only conversation, nothing below built yet)

A lot got discussed/decided in conversation without any code written this
round -- consolidated here as one list so a fresh session doesn't have to
re-derive it from scratch or lose pieces. Rough priority order, per the
user's own framing ("fix current bugs first, test-tone/real-BT-audio
last"):

1. **Verify the FLAC maxFrameSize patch actually works.** First real
   flash since `scripts/patch_audioI2S.py` landed (see "Open question...
   FLAC decode limitations" section above, option 3) -- confirm the
   `extra_scripts` hook fires (`[clickpod patch] ... applied` in the
   build log) and that a file which previously hit `maxFrameSize too
   large` (e.g. the TOOL/Lateralus one) now plays instead of skipping.
2. **Convert the build-time patch into a real fork.** User finds the
   build-script approach "clunky and slow" (their words) and wants
   something more proper -- the plan from earlier: user forks
   `schreibfaul1/ESP32-audioI2S` on GitHub themselves (this session
   couldn't get write access to do it from here -- see the "Open
   question" section's writeup), hands over the fork URL, then the same
   patch gets committed directly into the fork and `platformio.ini`'s
   `lib_deps` points at it (pinned to a specific commit/tag on the fork,
   not its default branch) instead of running `scripts/patch_audioI2S.py`
   at all -- that script gets removed once this lands.
3. ~~Real 24-bit FLAC decode support~~ -- **DECIDED AGAINST, not doing
   this.** Was briefly on the plan as the (explicitly flagged highest-
   risk) option #4, but the user reconsidered and is re-encoding their
   24-bit files to 16-bit instead (option #1 from the original FLAC-
   limitations list) -- batch-converting locally with `ffmpeg -map 0
   -c:v copy -sample_fmt s16 -c:a flac` (the `-map 0 -c:v copy` keeps
   embedded cover art intact, which a bare `-sample_fmt s16` conversion
   can otherwise drop). Lower-risk, same end result for their actual
   library. No firmware work needed for this item at all.
4. **Bluetooth device picker + last-device auto-reconnect.** See the
   corrected "Real Bluetooth toggle" section above -- `ESP32-A2DP`
   genuinely supports discovery (`start()`/`set_ssid_callback()`/etc.),
   this was wrongly assumed not possible before. Build: a device-list
   screen (reuses the Music/Playlists row pattern), the BT-stack-context-
   to-main-loop handoff for scan results (same pattern as the encoder
   ISR), and persisting the last-picked device name (`Persist.*`) so
   `main.cpp`'s boot-time auto-resume connects to whatever was last
   selected instead of the hardcoded `kTargetDeviceName` constant. User
   confirmed they want the auto-reconnect behavior specifically, not just
   the picker. Also: pin `ESP32-A2DP`'s `lib_deps` entry to a specific
   tag/commit while touching this file anyway -- it currently floats on
   the default branch, unlike the explicitly-pinned `ESP32-audioI2S`.
5. **SD-card-based WiFi credentials for TimeSync**, hardcoded-phone/
   laptop-hotspot style but kept OFF the repo per the user's explicit
   call (a single physical SD card in their pocket vs. permanent GitHub
   history -- their reasoning, and the right call for a repo that could
   go public). A small text file on the card (e.g. `clickpod_wifi.txt`,
   SSID/password pairs), read by `TimeSync` and tried alongside the
   existing open-network scan on each sync attempt -- no on-device typing
   UI needed, no UI exposure at all, user edits the file directly on
   their computer. Not an SD-index-file-style binary format like
   `/clickpod.idx` -- plain text is fine and easier for the user to hand-
   edit.
6. **Manual "Set time" UI** -- an analog clock face (hour/minute hands,
   set via encoder: tap to switch which hand is active, rotate to sweep
   it, CENTER to confirm) with a live digital readout underneath for
   actual glanceability (user's own reasoning: analog looks nice but
   isn't a quick read). Saves to NVS (`Persist.*`) + a `millis()`
   timestamp, counts forward between real syncs the same way `TimeSync`
   already does after a WiFi sync. Per this project's usual discipline,
   worth sketching in the browser simulator first (a real little
   animation/rendering job -- rotating hands cleanly, not static art) --
   see the simulator-first porting note near the top of this file.
7. **Sleep / "AOD" mode on long-press CENTER.** Currently
   `togglePower()`'s "off" fully blanks the screen and (implicitly, since
   nothing continues the UI loop meaningfully in that mode) doesn't
   really continue anything. Wanted instead: keep playback running
   exactly as-is, switch to a locked clock-face screen (reuses the
   digital readout from item 6), dim the backlight, and have input
   handling ignore everything except the wake gesture (CENTER long-press
   again, matching the existing in/out toggle) -- specifically to survive
   being tossed in a bag/pocket without accidental button presses
   skipping tracks or changing volume. The screen-dimming half needs the
   backlight rewiring below done first; the "keep playing + lock input +
   show clock" half is pure software, no hardware dependency.
   **Backlight hardware**: BL pin is currently hardwired to 3.3V (always
   full brightness, see the hardware-gotchas entry above). User confirmed
   it's a Waveshare ILI9341 module -- matches a known Waveshare board
   design (onboard transistor, commonly labeled Q1, dedicated to
   backlight switching -- confirmed via a real Bodmer/TFT_eSPI GitHub
   discussion about this exact board family, not just general ILI9341
   knowledge) -- the physical transistor the user found near the BL pin
   (described as a standard 3-pad package, one fat pad + two smaller
   forked pads) is very likely this same switching transistor. Good
   news: this means the BL header pin is probably already a logic-level
   control input (through a base resistor into that transistor), not the
   raw LED current line -- PWM dimming from a GPIO should be safe once
   rewired, likely without needing an extra MOSFET. Not 100% confirmed
   against this specific board's actual schematic, just a strong match.
   Still need: a free PWM-capable GPIO (budget is tight -- see the
   hardware-gotchas entry; GPIO0 is the live candidate, needs boot-strap-
   safety care) -- user is open to GPIO0, hasn't fully committed yet,
   worth confirming at the start of whichever session tackles this.
8. **Real Bluetooth audio** (routing `ESP32-audioI2S`'s decoded PCM into
   the A2DP source callback instead of the test tone) -- explicitly
   deprioritized by the user, do this LAST, after everything above.

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
