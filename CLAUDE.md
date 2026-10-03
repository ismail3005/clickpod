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
```

ESP32-audioI2S's FLAC maxFrameSize fix no longer lives in this repo at
all -- it's a real commit on the user's own fork
(`github.com/ismail3005/esp32-audioi2s`, `clickpod-3.0.12-flac-patch`
branch), pinned by commit SHA in `platformio.ini`'s `lib_deps`. See the
"real options for the two FLAC decode limitations" section below for the
full writeup of how this evolved from a build-time patch script to this.

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

**UPDATE -- real vector icons, replacing most of the plain letter
badges.** User explicitly asked for nicer icons on the 4 main-menu tiles
(only Bluetooth had a real hand-drawn glyph, Music/Playlists/Settings
were plain "M"/"P"/"S" letters) and mentioned track rows' letter badges
looked out of place too. `Screens.cpp` gained five new glyph-drawing
functions right next to `drawBtGlyph()`, same deliberately-simple-
primitives style (plain lines/circles/rounded-rects from a normalized
24x24 viewBox, nothing needing a curve library) so they can be reasoned
about for correctness without a way to preview them before the user
flashes them -- same discipline `drawBtGlyph()` already established:
- `drawNoteGlyph()` -- single eighth note (notehead + stem + flag).
  Music tile AND track rows both use this (a track IS a song -- same
  glyph at a different scale instead of inventing a second one).
- `drawPlaylistGlyph()` -- three descending-length horizontal bars, the
  standard "list" icon. Playlists tile and playlist rows.
- `drawSettingsGlyph()` -- three horizontal "sliders" (a line each with
  a knob at a different position), not a gear -- a gear's teeth need
  real resolution to read as teeth rather than a blob at this badge
  size, and getting that wrong with no preview was a real risk not
  worth taking for the Settings tile specifically.
- `drawArtistGlyph()` -- head (circle) + shoulders (rounded rect).
- `drawAlbumGlyph()` -- disc (outer ring + filled center hole).

`drawMainMenuGrid()`'s icon dispatch now branches on `"note"`/
`"playlist"`/`"gear"` to call the matching glyph (alongside the existing
`"bt"` branch) instead of printing a letter. `rowIconFor()`/
`drawRowIconIfAny()` (the shared per-row badge renderer every list
screen uses) gained a `RowGlyph` enum replacing the old single-letter
field, dispatching `"track"`/`"playlist"`/`"artist"`/`"album"`/`"bt"` to
real glyphs the same way -- Settings' own sub-rows
(`"brightness"`/`"sort"`/`"theme"`/`"timezone"`/`"rescan"`) stay plain
letter badges for now, not asked for this round and five more distinct
small icons was more new-shape surface area than seemed worth it in one
pass.

**Not yet hardware-confirmed**, same caveat as everything in this
sandbox (no PlatformIO to compile against) -- brace-balance and call-
site review done carefully since this touches shared rendering code
every screen depends on, but real visual confirmation needs a flash.
Next real step: flash, check the main menu's three newly-iconed tiles
plus a few track/playlist/artist/album rows render as expected (not
garbled/misaligned/wrong color) before trusting this pattern further.

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
3. **DONE, and CONFIRMED on real hardware -- `ESP32-audioI2S`'s FLAC frame
   buffer size, now a real fork commit, not a build-time patch script.**
   Investigated by actually cloning the real pinned 3.0.12
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

   **Originally** couldn't fork the library under the user's GitHub
   account from inside a session (session's GitHub access was scoped to
   `ismail3005/clickpod` only; both `mcp__github__fork_repository` and
   `add_repo` with push access to the external repo were refused --
   widening repo access needs the user's own explicit action). Shipped a
   **build-time patch script** instead first (`scripts/patch_audioI2S.py`,
   `platformio.ini`'s `extra_scripts` hook, string-replacing the
   downloaded library's `Audio.cpp` before every compile) as a stopgap --
   confirmed on the next real flash to actually work (every previously-
   failing `maxFrameSize too large!` file now plays), proving the patch
   itself was correct even though the delivery mechanism was clunky.

   **Now superseded**: the user forked `schreibfaul1/ESP32-audioI2S`
   themselves on GitHub (`github.com/ismail3005/esp32-audioi2s`, note
   GitHub's own redirect shows it canonically as
   `ismail3005/ESP32-audioI2S`), handed over the URL, and the same patch
   is now a real, permanent commit on a `clickpod-3.0.12-flac-patch`
   branch there, applied directly against the real pinned `3.0.12` tag
   (fetched from the real upstream, not the fork -- the fork's own clone
   didn't carry upstream's tags over, confirmed via `git ls-remote
   --tags origin` coming back empty; `git fetch upstream tag 3.0.12`
   pulled it in directly from `schreibfaul1/ESP32-audioI2S` instead).
   `platformio.ini`'s `lib_deps` now points at
   `https://github.com/ismail3005/esp32-audioi2s.git#ba0fa5a0d28538ebc9cf331043564c3496579878`
   -- pinned to that exact **commit SHA**, not a git tag: this session's
   push credentials could push the branch fine but hit a bare `HTTP 403`
   specifically on `git push origin <tag>` (tried both lightweight-tag-
   then-push and re-pushing against both the original and GitHub's
   redirected canonical-case repo URL, same 403 both times -- looks like
   a permission scope gap for tag refs specifically, not a transient
   error). A commit SHA pins exactly as hard as a tag would for
   `lib_deps`' purposes, so this wasn't worth chasing further.
   `scripts/patch_audioI2S.py` and the `extra_scripts` line are both
   **removed** -- no longer needed now that the same change lives as a
   real commit in a real dependency instead of being re-applied by a
   build script every time.

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

## Twelfth real hardware bug (found, fixed): track-start latency -- audio sat behind a JPEG decode

User reported pressing CENTER/RIGHT to start a track felt slow -- view
switched quickly but sound took "a sec" to actually begin. Root cause in
`MenuEngine.cpp`'s `setNowPlaying()`: it ran THREE blocking SD/SPI
operations -- `FlacMeta::readStreamInfo()`, `FlacMeta::readTags()`, and
`AlbumArt::loadForTrack()` (which includes a JPEG decode of the embedded
cover art) -- all before ever calling `AudioBridge::playSomething()`.
`AudioBridge::playSomething()` only needs the file path (see
`AudioBridge.h`), nothing from tags or art, so there was no real reason
for that ordering -- the actual decoder start was sitting behind however
long the slowest of those three (almost certainly the JPEG decode) took.

**Fixed** by reordering: only the cheap STREAMINFO read still happens
first (needed for duration + the existing 24-bit-skip check --
`knownUnsupported`, see the tenth hardware bug above), `
AudioBridge::playSomething()` is now called immediately after that,
and the slower tags/lyrics/art work moved to AFTER playback has already
been kicked off -- `state.now.artist`/`title`/`album` get a provisional
filename-derived value immediately (fast first redraw, sound starts),
then get overwritten with the real tag values a beat later once
`readTags()`/`AlbumArt::loadForTrack()` finish, with a `state.dirty =
true` at the end of the function to pick up that second update. Net
effect: sound starts right after the cheap duration read instead of
after a JPEG decode; the screen's tag/art details just arrive very
slightly later than before, which is the right tradeoff (cosmetic delay,
not an audio-start delay).

## Thirteenth real hardware bug (found, fixed): a latency follow-up attempt broke playback outright -- queue-destroying freeze

User still reported "significant delay" starting playback after the
twelfth bug's fix. Found a real remaining inefficiency: `FlacMeta::
readStreamInfo()` still ran before `AudioBridge::playSomething()`, each
independently opening the SAME file via two separate SD opens -- doubling
the SD directory-lookup overhead this project's own boot-time
measurements already established as the dominant per-file cost on this
card. **Fixed by moving `readStreamInfo()` into the deferred pass too**,
making `playSomething()` the unconditional first thing in
`setNowPlaying()`, nothing gating it at all.

**This broke playback.** User's next report: "when a song ends it
doesn't continue playing it just goes back to zero and freezes."
Mechanism: `readStreamInfo()` was now opening the same file a SECOND
time WHILE `playSomething()`'s `connecttoFS()` already had that exact
file open and actively decoding from it -- a genuine concurrent-file-
access hazard that didn't exist when the two calls were sequential. A
corrupted read from this race could spuriously report a bogus
`bitsPerSample`, falsely tripping the 24-bit-unsupported skip -- which
called `playNextInQueue()` -> `setNowPlaying()` on the next track WHILE
STILL INSIDE the original call. If that track's read got corrupted the
same way, it recursed, burning through the entire queue in one
synchronous burst until empty. The last `setNowPlaying()` in that
cascade had already reset `state.now.posSec` to 0 at its own top; the
empty-queue branch then set `state.now.playing = false`.
`tickPlaybackClock()` bails out immediately every tick once `playing`
is false, so nothing ever recovered -- exactly "goes back to zero and
freezes."

**Fixed by moving ONLY the STREAMINFO read back to before
`playSomething()`** -- correctness has to win over the latency cost of
one SD open before playback starts, full stop; there's no safe way to
keep the double-open fix for this specific read. Tags/art reading stay
deferred to after `playSomething()` (unchanged from the twelfth bug's
fix) -- unlike STREAMINFO, a bad tags/art read has no skip/recursion
path on failure, so it doesn't carry this same risk, and hasn't been
implicated in any reported freeze.

**Lesson**: the "avoid a double SD open" optimization was sound in
principle, but applying it to a read whose failure path can trigger
recursive, queue-destroying skip logic was not a safe trade -- a bad
read corrupting data is a materially different risk than a bad read
just failing cleanly (which `readStreamInfo()`'s normal "file not
found"-style failures already handle fine, durSec just stays 0/unknown).
Before deferring ANY SD read to run concurrently with an actively-open
decoder file handle, check what happens if that specific read returns
corrupted-but-plausible data, not just what happens if it cleanly fails.

## Fourteenth real hardware bug (found, REAL FIX shipped as a fork commit, not yet hardware-confirmed): heap corruption crash while scrubbing a FLAC track

User reported `CORRUPT HEAP: Bad tail` / `assert failed: multi_heap_free` after
scrubbing all the way through "Alice In Chains - Rotten Apple.flac" -- a real
crash/reboot, not the silent freeze from the thirteenth bug above (confirmed
on the latest build, after that fix). The log showed a string of transient
FLAC decode errors (`BITS PER SAMPLE > 16`, `BITS PER SAMPLE UNKNOWN`,
`UNKNOWN CHANNEL ASSIGNMENT`, each followed by "syncword found" resyncing)
right before the crash, which pointed at scrubbing/seeking rather than normal
playback.

**Investigated by actually cloning the real pinned `3.0.12` tag of
`schreibfaul1/ESP32-audioI2S`** (not our fork -- this needed the unmodified
upstream seek code) and reading `Audio::setAudioPlayPosition()` directly:

```cpp
bool Audio::setAudioPlayPosition(uint16_t sec) {
    uint32_t filepos = m_audioDataStart + (m_avr_bitrate * sec / 8);
    return setFilePos(filepos);
}
```

This is a pure average-bitrate estimate with **no FLAC frame-boundary
awareness at all** -- for FLAC's variable-length frames, it lands at an
essentially arbitrary byte offset almost every time. The library does have a
safety net (`flac_correctResumeFilePos()`, `Audio.cpp`) that scans forward
from that estimate for the next `0xFF 0xF8` syncword before resuming, then
calls `FLACDecoderReset()` + `InBuff.resetBuffer()` -- but FLAC's syncword is
only 16 bits, so against compressed audio data it has a real false-positive
rate. That's exactly what the log shows: a false "syncword" match, a garbage
frame header decoded from it (fake 24-bit/unknown-channel readings), the
decoder's own internal resync (`FLACFindSyncWord()`,
`flac_decoder.cpp`) kicking in again to find the real boundary. This
resync path only ever gets entered on a seek -- it's far less exercised than
normal straight-through decode, and the most plausible place for a real
library bug (this session didn't find a specific one, see below) to hide.

**Ruled out**: our own fork's patch (the FLAC `maxFrameSize` buffer-size fix,
in `read_FLAC_Header()`) is NOT re-entered on seek -- only `FLACDecoderReset()`
runs, which never touches that function. Confirmed by grepping the real
library source for every call site of `read_FLAC_Header()`. So this is a
pre-existing fragility in the library's own FLAC seek support, not something
our patch introduced.

**Real fix, not just a mitigation**: traced the exact mechanism in
`flacDecodeFrame()` (`flac_decoder.cpp`) -- a false sync match feeds garbage
field codes into the header parser, including a UTF-8-coded frame/sample-
number byte count (1-7 bytes) that gets blindly trusted with no validation
against the bytes actually remaining, which is a real out-of-bounds-read
shape, not just "decode errors in the log." The FLAC format defines a
frame-header CRC-8 byte for exactly this situation -- letting a decoder
verify it has truly landed on a frame boundary before trusting it -- and
this decoder (confirmed by grepping the whole `flac_decoder.cpp`/`Audio.cpp`
source) never implemented that check anywhere, for either normal resync or
seek-driven resync.

Fixed at the actual source, on the fork (`clickpod-3.0.12-flac-patch`
branch, commit `7fbb5c7`, re-pinned in `platformio.ini`): added
`Audio::flac_tryParseFrameHeader()`, a self-contained candidate-header
parser (doesn't touch `flac_decoder.cpp`'s live state, so it's safe to run
speculatively) that walks the full header -- block size/sample rate/channel
assignment/sample size codes (rejecting any reserved value), the variable-
length UTF-8 frame/sample number (validating every continuation byte's
shape instead of trusting the byte count blind), any optional extra size
bytes -- then computes the real FLAC frame-header CRC-8 (poly `0x07`,
MSB-first, no reflection, per the format spec) and compares it against the
byte actually on disk. `flac_correctResumeFilePos()` now keeps scanning
forward past a false positive instead of accepting the first 2-byte
`0xFF`/`0xF8` match it finds, which is what the original implementation
did (and still what upstream `schreibfaul1/ESP32-audioI2S` 3.0.12 does,
unpatched). This directly closes the "jittery/glitchy scrubbing" complaint
too, since every false-positive accept was previously producing audible
resync static.

**Still true, lower priority now**: `src/ui/InputRouter.cpp`'s
`kScrubIdleCommitMs` was also raised 150ms -> 400ms (previous session) so
one long scrub still fires fewer real seeks overall -- kept as a cheap
secondary win (every real seek still costs a resync scan, even a correct
one), not as the fix itself anymore.

**Not yet hardware-confirmed**: this fork commit was written and pushed in
this sandbox with no way to compile it here (no PlatformIO) -- same
verification discipline as every other library-source change in this
project. First real `pio run` + flash + a deliberate scrub-through-a-whole-
track test on "Alice In Chains - Rotten Apple.flac" (or another file known
to have triggered this) is the next real step, to confirm both that it
builds clean and that the crash is actually gone, not just less likely.

## Fifteenth real hardware bug (found, fixed): TimeSync WiFi sync could never run at all while Bluetooth was on

User reported WiFi NTP sync never connecting despite a correct
`/clickpod_wifi.txt` and their hotspot being on. Root cause in
`src/net/RadioLock.h`/`src/bt/BluetoothSource.cpp`: `BluetoothSource::
begin()` acquired `RadioLock` and only released it in `end()` -- i.e. for
BT's WHOLE connected session, not just the moment it actually touches the
radio. Since BT commonly stays on for hours, `TimeSync::tryOnce()`'s
`RadioLock::ScopedLock` could never acquire the lock for as long as BT was
on -- every attempt just logged `[time] skipping sync -- Bluetooth is
active` and retried in 2 minutes, forever, never actually reaching a WiFi
scan at all. Not a brief collision window as the lock's original comment
assumed -- a near-total, open-ended block.

Worth noting: this lock was originally added to guard against a suspected
WiFi/BT simultaneous-radio-use crash, but CLAUDE.md's own seventh/eighth
hardware-bug writeup already established that diagnosis was wrong (it was
internal-heap exhaustion, independently fixed by `radioHeapOk()`) -- so the
lock's blunt "whoever's active wins, for their whole session" design was
never actually validated as necessary in the first place, and was
silently breaking TimeSync by design.

**Fixed**: `BluetoothSource::begin()` now releases the lock immediately
after the actual radio-touching call (`a2dpSource.start()`) returns,
instead of holding it until `end()` -- same brief-hold pattern TimeSync
itself already uses (one scan+connect+NTP cycle, not "for as long as
WiFi might ever be wanted"). `end()` no longer calls `RadioLock::release()`
(it wasn't holding the lock that far anymore; releasing there risked
clearing `RadioLock::busy` out from under a TimeSync cycle that happened
to be running at that exact moment).

**Honest residual gap, not fully closed**: `ESP32-A2DP`'s `start()` is
asynchronous (kicks off scanning/pairing on its own BT task and returns
quickly), so the lock window here doesn't cover BT's whole multi-second
connection handshake, only the initial call -- a TimeSync scan could still
start a few seconds into that handshake. Given the original "simultaneous
radio use crashes" theory was never actually confirmed to be the real
cause of anything, this is judged an acceptable tradeoff for now rather
than a provably safe one. If a real WiFi/BT collision crash surfaces
again, this is the first place to revisit (probably by polling for a
stable `is_connected()` before releasing, instead of releasing right after
`start()`).

**Other possible contributing causes, not ruled out, worth checking if
this alone doesn't fix it**: (1) the ESP32's WiFi radio is 2.4GHz-only --
if the user's phone hotspot is set to 5GHz or "5GHz preferred", the device
can't see it in a scan at all, a common real-world phone default; (2)
`TimeSync.cpp`'s known-network match (`ssid == cred.ssid`) is an exact,
case-sensitive `String` comparison -- a typo or case mismatch between
`/clickpod_wifi.txt` and the hotspot's actual broadcast name silently
falls through to the open-network fallback (which a WPA2 hotspot will
never match either), with no specific "credential didn't match anything
in range" log line to point at it directly. Next real step either way:
get the actual boot-time serial log's `[time] ...` lines (credential
count loaded, scan result count, which SSID if any got chosen) rather than
guessing further blind -- this session fixed the one confirmed, provably-
real blocker (the lock) but hasn't seen a fresh log to confirm it was the
only one.

## Sixteenth real hardware bug (found, fixed): crash on track transition -- a FLAC decode-error-storm/garbage-pointer crash unrelated to scrubbing, plus a real reentrancy gap in AudioBridge

Confirmed the fourteenth bug's debounce (`kScrubIdleCommitMs`) genuinely
fixed scrubbing-while-paused -- user confirmed "pausing then scrubbing and
unpausing works great." But scrubbing *while playing* is still glitchy
(not yet separately investigated -- see the open item below), and a
**different** real crash hit at a track transition: `Guru Meditation
Error: Core 1 panic'ed (LoadProhibited)`, `EXCVADDR: 0xffffd986` -- a
classic garbage-pointer dereference, not a heap-corruption assert this
time. Two things stood out in the log:

1. The lead-up showed `[audio] playing: /funky times/Am...` truncated and
   repeated roughly 20 times in well under a second (garbled/overlapping
   UART output, not 20 literal distinct clean lines) before the real file
   ("America - A Horse with No Name.flac") actually finished initializing.
   That pattern -- `AudioBridge::playSomething()` (and therefore
   `audio->connecttoFS()`) being invoked many times back-to-back before a
   previous call had any chance to settle -- was a real, previously-
   unguarded gap: `AudioBridge.cpp` had zero protection against being
   called again mid-setup. This library isn't built to tolerate that.
   **What actually triggered the burst of calls was not conclusively
   identified this round** (a genuinely-bouncing button press and several
   menu-row actions firing in quick succession are both plausible) -- but
   regardless of the upstream trigger, calling `connecttoFS()` repeatedly,
   unthrottled, is unsafe on its own and worth guarding against
   unconditionally.
2. Once decode did start on the real file, the SAME class of decode-error
   storm as the fourteenth bug showed up again (`BITS PER SAMPLE UNKNOWN`,
   `RESERVED CHANNEL ASSIGNMENT`, `BLOCKSIZE TOO BIG`, repeated syncword
   resyncs) -- but this time at a **fresh track start, not a seek**. That
   proved the CRC-8 fix from the fourteenth bug wasn't the whole story:
   it only covered `flac_correctResumeFilePos()`, the seek-specific resync
   function. `FLACFindSyncWord()` -- the resync path entered any time
   `flacDecodeFrame()` hits ANY decode error mid-stream, seek or not --
   had the exact same false-positive-prone, unverified 2-byte `0xFF`/`0xF8`
   acceptance, just reachable without ever seeking.

**Two fixes, both real, not mitigations**:

1. `src/audio/AudioBridge.cpp`: `playSomething()` now ignores a call that
   arrives within `kMinMsBetweenPlaySomething` (150ms) of the previous one,
   logging why instead of silently dropping it. No legitimate human action
   needs a second real track-start inside 150ms, so this is a correctness
   guard against reentering the decoder library unsafely, not a feature
   limitation.
2. Fork (`clickpod-3.0.12-flac-patch`, commit `406e288`, re-pinned in
   `platformio.ini`): added the same structural-field + CRC-8 verification
   from the fourteenth bug's `flac_tryParseFrameHeader()` to
   `FLACFindSyncWord()` (`flac_decoder.cpp`) -- a separate, local copy
   (`flacTryParseFrameHeader()`/`flacFindSyncWordCrc8()`) since this path
   works directly on an in-memory buffer slice, not a file, so it needed
   its own implementation rather than sharing Audio.cpp's. A false 2-byte
   match is now skipped (scan continues) instead of trusted and handed
   back into the decoder, mid-stream resyncs included, not just seek
   resyncs.

**Not yet hardware-confirmed** -- same caveat as the fourteenth bug, no
PlatformIO in this sandbox to compile against. Next real step: flash,
confirm it builds, and specifically try to reproduce BOTH halves again --
rapid track selection (to confirm the reentrancy guard holds) and a full
playthrough to a natural track-end transition (to confirm the crash itself
is gone, not just the seek-triggered half of it).

**Still open, not investigated this round**: scrubbing while music is
actively playing is still reported as glitchy, distinct from the
now-confirmed-fixed pause-then-scrub-then-unpause path. Worth a closer
look at what's different about the live-playback case specifically (the
decoder is actively mid-decode when the seek lands, vs. idle when paused)
once the two fixes above are confirmed on hardware.

## Seventeenth real hardware bug/decision (reverted + replaced): seeking while playing should NOT pause, even briefly -- fixed the decoder instead, and fixed perceived skip latency separately

First attempt at this round's "scrub/skip glitchy while playing" report
(see below for the original signal that led here) was to bracket
`AudioBridge::seekTo()`/`playSomething()` with an invisible internal
pause/resume around the real decoder call. **User explicitly rejected
this**: "no i dont want my music to pause while scrubbing... i want to be
able to scrub while its playing and then land on the bit i want and have
it go there" -- any audible interruption during the one real commit, even
brief, defeats the point. **Reverted** -- `seekTo()`/`playSomething()` are
back to calling the decoder directly, no bracket.

The actual right fix for the underlying hazard (seeking/switching tracks
while the decoder is actively live was genuinely unsafe on this library)
is the CRC-8 frame-header verification already shipped on the fork for
BOTH the seek-path (`flac_correctResumeFilePos()`, fourteenth bug) and
the mid-stream resync path (`FLACFindSyncWord()`, sixteenth bug) -- that
fixes the decoder's own resync robustness so it's safe to touch live,
rather than avoiding ever touching it live. This is the right place for
this fix to live; the pause-bracket was treating a decoder bug as a UI
workaround. Still not hardware-confirmed at the time of this entry (see
those two writeups).

**Separately, real fix for "LEFT/RIGHT skip feels slow"** -- user
clarified this one is NOT glitchy, just laggy ("button delay again").
Root cause in `MenuEngine.cpp`'s `setNowPlaying()`: `state.dirty` was
only ever set ONCE, at the very end of the function, AFTER the slower
deferred work (`FlacMeta::readTags()` -- a real SD read -- and
`AlbumArt::loadForTrack()` -- a JPEG decode). Audio itself starts early
(the twelfth hardware bug already fixed THAT latency), but the screen
sat on the OLD track the whole time that deferred work ran, since nothing
ever told it to redraw sooner -- and the screen, not audio start time, is
a user's main cue for "did my button press register," so this read as
sluggish regardless of how fast playback itself actually started.

Just moving `state.dirty = true` earlier in the function wasn't enough on
its own and would be worth remembering as a near-miss: `setNowPlaying()`
runs entirely synchronously inside one `UI::update()` call (`InputRouter::
update()` -> ... -> `setNowPlaying()`), and `Screens::render()` is only
invoked once, at the very END of that same `UI::update()` call -- so
setting the flag earlier doesn't matter if the deferred work still runs
before the one `render()` call that would act on it. Fixed by explicitly
calling `Screens::render()` right after the flag is set (immediately
after kicking off `AudioBridge::playSomething()`, with the provisional
filename-derived title already in `state.now.*`), forcing the screen to
actually flip to the new track before the deferred tag/art work runs,
not just scheduling it to. `state.dirty` still gets set again at the
original spot at the end of the function, picking up real tags/art in a
second, cheap render pass shortly after -- same "provisional now, real
value a beat later" pattern this codebase already uses elsewhere, just
now with two real redraws instead of one silently-delayed one.

**Not yet hardware-confirmed**, same caveat as every change this round.
Next real step: flash, confirm scrubbing stays audibly uninterrupted
while playing, and that LEFT/RIGHT now visually responds immediately
even before the real tags/art catch up a moment later.

## Eighteenth real hardware bug (diagnostics added, not yet root-caused): TimeSync still ignoring the user's hotspot, picked a public open network instead

Fresh log confirmed the fifteenth bug's fix (narrowed `RadioLock`) is no
longer the blocker -- `tryOnce()` now actually reaches a WiFi scan and
join attempt (`[time] joining "freewifi-epfl" for NTP (open network)...`).
But it picked a public open campus network instead of the user's own
hotspot entirely -- exactly what happens when `knownCredentials` is still
empty, which the fifteenth bug's writeup already flagged as unconfirmed:
the credentials-match loop never even got a chance to try the hotspot,
so the open-network fallback grabbed whatever open network happened to
be in range. Root cause of the empty credential list itself still not
pinned down -- this session has no way to see the actual SD card content.

`[time] NTP fetch failed` after `ASSOC_LEAVE` also shows: even the open
network it DID pick didn't actually get usable NTP time (joined then
immediately dropped, or joined but the NTP request itself failed) --
a second, distinct failure layered on top of "wrong network chosen,"
not yet investigated either.

**Added real diagnostics instead of guessing further** (`TimeSync.cpp`):
1. `loadCredentialsFromSd()` now logs the file's byte size, every raw
   line read (length + content) before any filtering, and strips a
   leading UTF-8 BOM (`EF BB BF`) if present -- a common artifact of
   saving plain text as "UTF-8 with BOM" (e.g. Windows Notepad's "UTF-8"
   option), which would otherwise corrupt the first line's SSID. The
   final "loaded N credentials" line now also reports how many raw lines
   were read, to distinguish "file is empty" from "file has lines but
   none parsed."
2. `tryOnce()` now logs every SSID actually seen in each scan (plus
   open/secured) before deciding what to join -- directly answers
   whether the hotspot is even visible to this 2.4GHz-only radio at all
   (a real possible cause flagged in the fifteenth bug's writeup: many
   phone hotspots default to 5GHz, invisible to this chip regardless of
   credentials) versus a credentials-matching problem.

**Next real step**: get a fresh boot log with these new lines -- that
will show definitively whether the hotspot even appears in scan results,
and if the credentials file has real content, exactly what's failing to
parse from it, rather than guessing at formatting again.

## Nineteenth real hardware bug (found, fixed): a SECOND, deeper FLAC decoder bug behind "scrubbing while playing does nothing" -- STREAMINFO values wiped on every reset

User reported the sixteenth/seventeenth bugs' fixes made no difference --
scrubbing mid-playback still produced the same decode-error storm
(`readUint(): error in bitreader` repeated, then `UNKNOWN CHANNEL
ASSIGNMENT` / `BITS PER SAMPLE UNKNOWN`). The new log was actually good
evidence the CRC-8 fix WAS working, not failing: `Channels: 2`/
`SampleRate: 44100`/`BitsPerSample: 16` were printed correctly right
before each failure, meaning a real, CRC-8-verified frame header was
found -- the false-positive-match bug those fixes targeted is a different
bug from this one.

Traced the real cause by reading `flacDecodeFrame()` (`flac_decoder.cpp`)
again, closely: `FLACMetadataBlock->numChannels`/`sampleRate`/
`bitsPerSample` are each only ever SET from a frame's own header when
that frame's own code is non-zero (`if(!FLACMetadataBlock->bitsPerSample)
{ if(sampleSizeCode==1) ... }`, etc.) -- this is correct per the FLAC
spec, where a per-frame code of 0 legitimately means "same as file-level
STREAMINFO," relying on the decoder already having that value cached
from processing a PRIOR frame. The bug: `FLACDecoderReset()` ->
`FLACDecoder_ClearBuffer()` `memset`s the ENTIRE `FLACMetadataBlock_t`
struct to 0 on every reset -- and this reset runs not just on a seek, but
on ANY resync, including the ordinary mid-stream one from
`FLACFindSyncWord()` (the sixteenth bug's fix target). Right after a
reset there's no "prior frame" to have cached these from, so a
completely valid frame whose header uses the common "inherit from
STREAMINFO" 0-codes has nothing to inherit, and the decoder incorrectly
reports `BITS PER SAMPLE UNKNOWN`/`UNKNOWN CHANNEL ASSIGNMENT` on a file
that's perfectly fine. This is a second, independent bug from the false-
positive-syncword one -- fixing header validation (CRC-8) was necessary
but not sufficient, since even a header that's 100% genuinely real still
hits this.

**Fixed** at the real source (fork, `clickpod-3.0.12-flac-patch`, commit
`256bd3b`, re-pinned in `platformio.ini`): `FLACDecoderReset()` now saves
`numChannels`/`sampleRate`/`bitsPerSample` before clearing
`FLACMetadataBlock`, and restores them after -- these are real file-level
properties that don't change mid-file, so carrying them across any reset
(seek, loop-to-start, or ordinary resync) is correct, not just a
workaround for the seek case specifically. Fixed centrally in
`FLACDecoderReset()` itself rather than at each call site (an earlier,
reverted attempt patched the seek-resume call site in `Audio.cpp`
directly via the already-existing `FLACSetRawBlockParams()` setter, but
that would have missed the mid-stream `FLACFindSyncWord()` reset path
entirely -- centralizing in `FLACDecoderReset()` covers every call site
with one change, current and future). The very first reset ever for a
file (before STREAMINFO has been parsed at all) just preserves 0->0, a
no-op -- `read_FLAC_Header()` unconditionally overwrites these three
fields with the real STREAMINFO values right after, unaffected.

**Not yet hardware-confirmed**, same caveat as every fork change this
round. Next real step: flash and scrub mid-playback on a file that
previously triggered this (any real FLAC should now do, this wasn't file-
specific) -- expect the on-screen position to actually relocate instead
of audio continuing to just play through as if nothing happened, which
is what "literally does nothing" meant: the backend seek was firing, but
the resumed decode was failing immediately after, every time.

**UPDATE, user confirmed on real hardware after flashing this fix**: the
freeze/no-relocate bug IS actually fixed now -- but scrubbing during
active playback is still laggy (not glitchy/crashy anymore, just slow to
respond). **User explicitly decided to drop this and move on** --
parked, not resolved. If picked back up later: this is a performance/feel
complaint now, not a correctness bug, so the next angle would be
measuring where the real time goes in the seek path (the resync scan in
`flac_correctResumeFilePos()` reads the file byte-by-byte via
`audiofile.seek()+read()` per candidate, which could plausibly be slow
on this SD card if many false-adjacent candidates get scanned before a
CRC-8 match lands -- not confirmed, just the obvious next place to look)
rather than guessing at another decoder correctness bug.

**UPDATE, confirmed resolved**: user confirms scrubbing is now perfect on
the latest flash (the twenty-fifth bug's rubberbanding fix, above --
`state.scrubPending` suppressing the real-position sync fight during an
active scrub -- was the fix that actually closed this out; the "laggy"
complaint here was most likely the same rubberbanding fight read as
sluggishness, not a separate unaddressed seek-performance issue after
all). No further action needed on scrubbing.

## Bluetooth device picker + last-device auto-reconnect (built, not yet hardware-confirmed)

Plan item 4 from the "Next session plan" above -- the Bluetooth screen's
single hardcoded-target row is still there, but it's joined by a real
"Choose device..." row that does genuine discovery, not a mock list.

**`src/bt/BluetoothSource.*`**: `startDiscovery()` calls `a2dpSource.
start()` with no name (confirmed from the real library source this
begins a scan instead of connecting to a fixed target) and installs a
`set_ssid_callback()` that stashes each newly-seen, not-yet-seen device
name into a small fixed-size array (`kMaxDiscovered = 24`, plain `char[32]`
rows, no heap allocation) and always returns `false` -- never auto-
selects, this is pure listing. That callback runs on the BT stack's own
task context, not the main loop (same constraint `AnoInput`'s encoder ISR
already has), so it can't touch `MenuEngine`/`state` directly -- the main
loop drains it via `discoveredCount()`/`discoveredName()`, a plain
volatile-int-gated array, single producer (the callback) / single
consumer (the main loop), append-only, same risk profile already
accepted for the encoder delta accumulator. `connectToDiscovered(name)`
cancels discovery and calls the existing `begin(name)` path (unchanged --
still has the heap guard, still the brief `RadioLock` hold), so picking a
device reuses every safety check already proven for the hardcoded-target
path, not a new, separately-risky connect routine.

**Confirmed from the real library source, not guessed**: `start()`
checks `esp_bluedroid_get_status()` and only runs the heavy Bluedroid/
controller init if it's still `UNINITIALIZED` -- calling it twice in one
session (once for discovery, once for the real connect once a device's
picked) does NOT redundantly re-init the whole BT stack. De-risks the
scan-then-connect flow below a fair amount, though the full discovery->
connect transition still isn't hardware-tested.

**UI (`MenuEngine.cpp`)**: `enterBluetoothDevicePicker()` starts discovery
and pushes a SECOND menu level while staying in `AppMode::BT` (no new
`AppMode` needed -- `AppMode::BT` already renders via the same generic
menu-stack machinery as `AppMode::MENU`, it just controls return-routing
and the title-bar glyph, so a second stack level Just Works with zero
`Screens.cpp` changes). `refreshBluetoothDevicesMenu()` rebuilds that
screen's rows from whatever's been stashed so far, preserving the
current selection across a rebuild; `UI.cpp`'s new `tickBluetoothDevice
Picker()` polls `discoveredCount()` once per `update()` tick (cheap int
compare) and only calls the rebuild when it actually changed, while that
specific screen is open. `InputRouter.cpp`'s LEFT handling for
`AppMode::BT` now pops one menu level at a time (matching the existing
`TRACK_MENU` pattern) instead of always jumping straight out of
Bluetooth in one press, and cancels any in-progress scan when backing out
of the picker specifically.

**Persistence (`Persist.*`/`AppState.h`)**: `state.btDeviceName` (new,
empty by default) is set and saved the moment a device is picked, and
`main.cpp`'s boot-time auto-resume now uses it when non-empty, falling
back to the old hardcoded `BluetoothSource::kTargetDeviceName` otherwise
-- this is the "stays paired" equivalent this project settled on earlier,
since classic A2DP has no persistent OS-level bonding the way phones do.
`syncBluetoothToUi()` (`main.cpp`) now reads the actual connected target
via the new `BluetoothSource::currentTargetName()` getter instead of
always assuming the hardcoded constant.

**`platformio.ini`**: `ESP32-A2DP`'s `lib_deps` entry is now pinned to a
commit SHA (`35bace5`) -- confirmed via `git tag -l` that this repo has
no tags at all, so a SHA is the only real pin available, same situation
`ESP32-audioI2S` was in before that one got its own fork. Previously
floated on the default branch's HEAD.

**Not yet hardware-confirmed, same caveat as every change this session**:
no PlatformIO in this sandbox to compile against. Specifically unverified
on real hardware: the scan-and-list UI actually populating with real
nearby device names, picking one and having it actually connect (not just
compile), and the boot-time auto-resume picking up a previously-chosen
device correctly after a reboot.

## Twentieth real hardware bug (found, fixed): real BT reconnect without re-pairing + a tiered heap threshold, from real post-flash numbers

User flashed the device-picker round and reported two things from a real
serial log: (1) headphones that had connected successfully several times
still needed to go back into pairing mode every single time, and (2) "we
have a memory issue man" -- `radioHeapOk()` logging free internal heap as
low as 0 bytes, and a sustained ~39-40KB baseline well under the 60KB
floor, meaning every Bluetooth operation after the first discovery scan
kept getting silently refused.

**Reconnect fix**: read the real `ESP32-A2DP` source again and confirmed
it already has NVS-backed bonding built in --
`set_last_connection()`/`get_last_connection()`/`last_bda_nvs_name()`
store a bonded device's address in NVS, and `bt_app_av_sm_hdlr()`'s
`APP_AV_STATE_IDLE` handler checks `reconnect_status == AutoReconnect &&
has_last_connection()` -- if true, it connects DIRECTLY to the stored
address, skipping the name-based discovery scan entirely (which is the
only reason a device needs to be in pairing/discoverable mode in the
first place -- that's only required to be FOUND during a fresh inquiry
scan, not to accept a direct reconnect from an already-bonded peer).
**`reconnect_status` defaults to `NoReconnect`** (confirmed in
`BluetoothA2DPCommon.h` -- the library's own header comment claiming
"per default this is on" is wrong/stale), and this codebase never
enabled it, so every single connect -- even to a device that had paired
successfully many times before -- always did a fresh name-scan requiring
pairing mode. This is the same class of thing as the "doesn't enumerate
discoverable devices" claim corrected earlier this session: the library
supports more than this codebase assumed.

Fixed: `BluetoothSource::begin()` gained an `allowAutoReconnect` parameter
(default `true`), calling `a2dpSource.set_auto_reconnect(allowAutoReconnect)`
before `start()`. The normal paths (status-row tap, boot auto-resume)
use the default -- a bonded device reconnects silently, no pairing mode
needed. The device-picker's `connectToDiscovered()` explicitly passes
`false`: auto-reconnect's stored-address branch would otherwise IGNORE
the picked name entirely and silently reconnect to whatever was bonded
before, if anything -- forcing a real name-based scan here is also what
lets the library's own success handler update its stored address to the
newly-picked device, so the next normal `begin()` call correctly
auto-reconnects to that device from then on.

**Heap threshold fix, from real numbers, not another guess**: the
original single `kMinInternalHeapForRadio` (60KB) assumed WiFi/BT always
need the same conservative floor. The real log showed something
different: the FIRST-ever Bluedroid init this session (while heap was
still high, ~128KB) succeeded and permanently dropped free internal heap
to a STABLE ~39-40KB baseline -- not trending down further (not a leak),
just Bluedroid's own resident footprint for the rest of the session. BT
kept running/scanning fine at that level, no crash -- but the ORIGINAL
60KB floor meant every operation after that first one (reconnect,
retry, a second discovery scan) saw ~40KB, failed the 60KB check, and
silently refused forever. The threshold itself was the bug, not a real
hardware limit.

Split into two: `kMinInternalHeapForRadioColdInit` (60KB, unchanged --
for before Bluedroid has ever been initialized this session, where the
actual documented crashes happened) and `kMinInternalHeapForRadioWarm`
(25KB -- for once it's already up, based on the confirmed-OK ~39-40KB
observed baseline, with real margin under it). `radioHeapOk()` now
checks `esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED` to
pick which floor applies -- WiFi has no equivalent "already paid its
init cost" signal available here, so it always uses the conservative
cold-init floor regardless; this mainly un-sticks repeated BT operations
after the first one. `esp_bt_main.h`'s `esp_bluedroid_get_status()` is
core ESP-IDF API, not independently header-verified in this sandbox (no
IDF headers available here), same confidence caveat as
`esp_bt_controller_mem_release()` used earlier this session.

**PSRAM pushed harder, system-wide, in response to "use the psram to
offload pressure"**: `main.cpp`'s `heap_caps_malloc_extmem_enable()`
threshold lowered from 4096 to 128 bytes. The 4096 threshold (set
earlier this session) only ever caught genuinely large allocations (a
big opened playlist's track vector, a lyrics buffer) -- it never touched
the much more common SMALL ones: every individual `Track`'s artist/
album/title/path `String`, each comfortably under 4KB alone but numerous
(up to hundreds live at once while a big playlist/album screen is open,
via the on-SD index materializing that one screen's tracks). Arduino's
`String` has no small-string optimization -- any non-empty one
allocates its buffer from the heap immediately -- so this is a real,
previously-unaddressed chunk of internal RAM this change moves to PSRAM
automatically, system-wide, instead of needing every call site hunted
down by hand. Same safety argument as before applies unchanged:
DMA-capable allocations explicitly request `MALLOC_CAP_DMA`/`INTERNAL`
and bypass this threshold regardless, so WiFi/BT/I2S's own buffers are
unaffected either way -- if audio or Bluetooth output gets audibly
glitchy/corrupted after this (not just "fails to start," which the
existing heap guards already handle safely), this is the thing to
suspect and revert first.

**Not yet hardware-confirmed**, same caveat as every change this
session. Next real step: flash, confirm the headphones connect WITHOUT
needing pairing mode this time (should just reconnect silently to
whatever was bonded last), and check whether the heap numbers in a fresh
log look healthier across a longer session (multiple BT operations, not
just the first one).

**Still open, not addressed this round**: the WiFi hotspot still isn't
connecting -- user confirms the credentials file content should be
correct on their end (named it themselves, has the password), and the
scan list shows a plausible candidate (`"Idephics2"`, a personal-looking
name unlike the institutional `epfl`/`eduroam` entries also in range) --
but the diagnostic lines added for the eighteenth bug (file size, every
raw line read from `/clickpod_wifi.txt`) weren't included in the pasted
log excerpt (it started mid-session, not from a fresh boot), so this
still isn't actually confirmed either way. Next real step: get those
specific lines from a fresh power-on, not guess further.

**UPDATE -- resolved, not a firmware bug.** A fresh full boot log showed
the eighteenth bug's new diagnostics clearly: `[time] /clickpod_wifi.txt:
0 bytes` and `0 raw line(s) read`. Turned out to be a real-world editing
mistake, not a parsing bug: the user had typed the credentials into
Notepad but never saved (no Ctrl+S) before ejecting the SD card -- the
file on disk was genuinely 0 bytes, but reopening it in Notepad
afterward silently restored the unsaved buffer from Notepad's own
autosave/session-recovery, making it look full when checked on the
computer. The diagnostics did their job (pinpointed "file is empty",
exactly per the eighteenth bug's reasoning) -- nothing to fix in
`TimeSync.cpp`.

## Twenty-first real hardware bug (found, real root cause confirmed by reading library source, fix shipped as an app-level workaround): Bluetooth reconnect always needed pairing mode again, even right after a successful connect

Same full boot log above also captured a complete BT off/on/off/on cycle
with no device-picker involved (plain status-row toggles), and it
conclusively answered the diagnostic question from the twentieth bug's
writeup: the log did NOT show `"Reconnecting to %s"` on the second
"Bluetooth On" -- it also never showed `"No last connection found,
disabling auto reconnect"` either, which was the real clue.

Traced the exact mechanism by cloning the real `ESP32-A2DP` library
source (not guessed) and reading `BluetoothA2DPCommon.cpp`/
`BluetoothA2DPSource.cpp` directly:

- `BluetoothA2DPCommon::end()` unconditionally calls
  `clean_last_connection()` (`end()`'s very first real action, before
  even disconnecting) -- which calls `set_last_connection()` with an
  all-zero address. This overwrites BOTH the in-RAM `last_connection`
  member AND the library's own NVS blob (namespace `"connected_bda"`, key
  `"src_bda"` for Source mode, confirmed directly from
  `write_address()`'s real `nvs_open()`/`nvs_set_blob()` calls) --
  **every single time Bluetooth is stopped**, even if it had just bonded
  successfully moments earlier. This is a genuine library bug/design
  flaw: stopping a session and permanently forgetting the bonded device
  are conflated into one call.
- The reason the SECOND "Bluetooth On" showed neither log line: by then
  the NVS blob exists (created by the first successful write) and
  contains a validly-stored but **all-zero** address. `start()`'s
  `get_last_connection()` -> `read_address()` -> `nvs_get_blob()`
  succeeds (it's a real, present blob, just zeros), so `start()` never
  logs "no last connection" -- but 10 seconds later, `av_hdl_stack_evt()`
  checks `has_last_connection()` against the now-zeroed in-RAM value,
  which is false, so it silently falls through to a fresh discovery scan
  (`"Starting device discovery..."`) requiring pairing mode -- exactly
  matching the observed log, mechanism fully confirmed start to finish.

**Fix, app-level, no fork needed this time** (`src/bt/BluetoothSource.cpp`):
`set_last_connection()`/`get_last_connection()`/`has_last_connection()`/
`last_bda_nvs_name()` are all `protected` in the library -- can't be
called from application code -- but `get_last_peer_address()` (returns
the live `esp_bd_addr_t*`) is public, and the library's own NVS
namespace/key names are stable implementation details now confirmed from
source. `BluetoothSource::end()` now reads the real bonded address via
`get_last_peer_address()` and saves it to our OWN NVS slot
(`"cpod_bt"`/`"last_bda"`, survives a reboot too) BEFORE calling the
library's `end()` (which immediately zeroes its own copy, as above).
`BluetoothSource::begin()`, when `allowAutoReconnect` is true (skipped
for the explicit device-picker path, which wants a genuine fresh scan),
re-seeds the LIBRARY's own NVS blob (`"connected_bda"`/`"src_bda"`)
directly with that saved address right before calling `start()` -- so by
the time the library's internal `get_last_connection()` runs, it finds a
real address again instead of the zero one `end()` just wrote, and the
10-second-later `av_hdl_stack_evt()` check correctly takes the
`"Reconnecting to %s"` direct-address path instead of discovery.

**Not yet hardware-confirmed** -- same caveat as every change this
session, no PlatformIO in this sandbox to compile against. Next real
step: flash, do a plain Bluetooth-off-then-on cycle (no device picker)
on an already-bonded device, and check the log specifically for
`"Reconnecting to %s"` on the second "on" instead of
`"Starting device discovery..."` -- that's the one line that confirms
this actually worked, not just compiled.

**UPDATE, confirmed on real hardware (same-session case)**: user paired
once, disconnected, turned Bluetooth back on, and it reconnected
immediately with no pairing mode needed. The shadow-save/reseed fix
works for the case it was built for.

**UPDATE, confirmed on real hardware (power-cycle case too)**: user
confirmed BT also reconnected without pairing mode after a RST press and
separately after a full unplug/power-cycle -- the shadow's own NVS
persistence (`"cpod_bt"`/`"last_bda"`) genuinely survives a reboot, not
just an in-session off/on toggle. This bug is fully closed out; both
halves of the fix (same-session and cross-reboot) are hardware-confirmed
now, not just theorized from source.

## Twenty-second real hardware bug (resolved, not a firmware bug): WiFi hotspot genuinely wasn't broadcasting during the earlier failing boots

Chased this across several rounds (fifteenth/eighteenth bugs above) --
resolved by just getting a fresh log with the hotspot confirmed ON
beforehand: `[time] scan found 15 network(s)...` now included
`"ISMAIL-LAPTOP 6786" (secured)`, it joined it as a known network, and
logged `[time] synced`. The whole WiFi/NTP pipeline (credentials file
parsing -> scan -> known-network preference -> WPA join -> NTP fetch)
works correctly end to end -- every earlier failing attempt really was
just the hotspot not actually being on/broadcasting at boot (confirmed
2.4GHz-only already, for the 3D printer, so the band theory from the
twentieth bug's writeup was a red herring for this specific network).
Nothing to fix in `TimeSync.cpp`.

## Twenty-third real hardware bug (found, fixed): statusbar clock never updated on its own, even after a successful sync

Same session: `[time] synced` appeared in the serial log, but the clock
never appeared on screen. Root cause in `Screens.cpp`: `drawStatusbar()`
(which prints `TimeSync::currentTimeString()`) is only ever called
inside `render()`'s `if (!state.dirty) return;`-gated block -- i.e. only
as a side effect of some OTHER full-screen redraw (a mode change, a
track change, boot finishing, ...). Nothing anywhere ever set `state.dirty`
purely because time passed or because `TimeSync::isSynced()` flipped
true in the background -- so if the user just sat on one screen without
pressing a button, the clock stayed frozen at whatever it showed during
the last full redraw (typically still `"--:--"` from before sync
completed), indefinitely, no matter how long ago the sync actually
finished.

Fixed with the same lightweight-dirty-flag pattern this codebase already
uses for the progress bar and menu selection (`state.progressDirty`/
`state.selectionDirty`): `AppState.h` gained `state.statusbarDirty`.
`UI.cpp`'s new `tickStatusbarClock()` (called every `update()` tick,
alongside `tickPlaybackClock()`) compares the current
`TimeSync::currentTimeString()` against the last value it saw (a plain
`String` compare, cheap) and only sets the flag when it actually changed
-- sync completing, or a new minute ticking over. `Screens::render()`
redraws just the statusbar (not the whole screen) when the flag is set
and a full `dirty` redraw isn't already happening, skipped for `BOOT`
(splash may not have drawn the statusbar yet) and `OFF` (that screen is
deliberately blank). Same risk profile as the existing progress-bar/
selection partial-redraw paths -- a small `fillRect` of just the top
strip, not a source of the flicker class of bug those were built to
avoid.

**Not yet hardware-confirmed** -- same caveat as every change this
session, no PlatformIO in this sandbox to compile against. Next real
step: flash, sit on any screen (Now Playing, a menu, Lyrics) without
touching a button, and confirm the clock in the statusbar actually
advances/appears on its own once a sync completes or a minute ticks
over, instead of needing an unrelated button press to reveal the
already-correct time.

## Twenty-fourth real hardware bug (diagnosed, not yet fixable blind): WiFi join fails with alternating NO_AP_FOUND/AUTH_EXPIRE, password/mode suspected

Next log after the clock fix confirmed the hotspot IS now seen in scan
results (`"ISMAIL-LAPTOP 6786" (secured)`, chosen as a known network) --
but the actual join attempt fails repeatedly:
```
[time] joining "ISMAIL-LAPTOP 6786" for NTP (known network)...
Reason: 201 - NO_AP_FOUND
Reason: 2   - AUTH_EXPIRE
Reason: 201 - NO_AP_FOUND
Reason: 2   - AUTH_EXPIRE
Reason: 201 - NO_AP_FOUND
[time] couldn't join in time, will retry later
```
Read `TimeSync.cpp`'s `tryOnce()` again end to end -- the join call itself
(`WiFi.begin(ssid.c_str(), password.c_str())`) is unremarkable, standard
Arduino-ESP32 API, nothing wrong in our own code. This alternating
NO_AP_FOUND/AUTH_EXPIRE flapping during repeated internal reconnect
attempts is a well-known ESP32 WiFi-stack symptom for either (a) a wrong
password, or (b) an auth-mode incompatibility -- some ESP32 Arduino core
versions have real trouble with WPA2/WPA3-transition ("mixed") mode,
which Windows 11's Mobile Hotspot can default to depending on OS build/
adapter, producing exactly this "finds the AP, tries to auth, drops,
retries" pattern rather than a clean single rejection.

**Can't distinguish between these two causes from here** -- no way to
test a real WPA join from this sandbox. Two things worth trying on the
user's end, in order of ease:
1. Double-check the password was typed into `/clickpod_wifi.txt`
   byte-for-byte correctly -- `"h|1P0946"` contains a pipe character
   (`|`), an easy one to mistype, mis-paste, or have a font/keyboard
   layout render ambiguously. Worth a very literal re-check.
2. If the password is confirmed correct, check whether Windows' Mobile
   Hotspot has any security-mode option beyond the default (some builds
   expose WPA2 vs. WPA2/WPA3 under advanced network settings) -- forcing
   plain WPA2-Personal, if available, would rule out (or fix) the
   mixed-mode theory.
Not attempted: guessing at an `esp_wifi`-level auth-mode override from
here -- that's exactly the kind of blind library-API guess this project
avoids (see the FlacMeta/AlbumArt confidence-level precedent above);
needs a real header/behavior check this sandbox can't do for WiFi
specifically (unlike `configTime()`/`getLocalTime()`, which are
well-trodden enough to use directly).

**UPDATE, real fix applied (password confirmed correct by the user,
ruling out typo theory) -- `TimeSync.cpp`'s `tryOnce()`:**

1. `WiFi.setSleep(false)` before connecting -- WiFi modem power-save
   missing beacons mid-handshake is a well-documented real cause of
   exactly this `AUTH_EXPIRE` flapping on ESP32.
2. The scan loop now also captures the chosen network's `WiFi.channel(i)`/
   `WiFi.BSSID(i)` from the SAME scan that already found it successfully,
   and `WiFi.begin()` is called with that explicit channel+BSSID instead
   of just SSID+password. Without this, `WiFi.begin(ssid, pass)` makes
   the ESP32 run a SECOND internal scan to locate the AP before
   authenticating -- a second discovery pass that can behave differently
   from the one this function just ran cleanly, which is the standard,
   well-documented fix for "found it in my own scan, but `WiFi.begin()`
   still can't join" on this chip. Both `channel()`/`BSSID()` and the
   4-arg `WiFi.begin(ssid, pass, channel, bssid)` overload are long-
   standing, stable Arduino-ESP32 core API (not a guess the way an
   `esp_wifi`-level override would have been) -- this is why option was
   taken over guessing at a lower-level auth-mode API.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox. Next
real step: flash, and check whether the join succeeds this time (no
more alternating `NO_AP_FOUND`/`AUTH_EXPIRE`, `[time] synced` appears).
If it still fails the same way with a confirmed-correct password and a
direct BSSID join, the WPA2/WPA3 mixed-mode theory becomes the leading
suspect and the next step is checking Windows' Mobile Hotspot for a
security-mode override, not another firmware guess.

**UPDATE -- this fix caused a real bootloop, REVERTED.** The commit
right after this one (`fbd8d84`) was the only change between a
confirmed-working flash and one that bootlooped on every power-on. User
reverted to the pre-fix commit (`c8a65e1`) and confirmed **plain
`WiFi.begin(ssid, pass)` with no channel/BSSID now joins successfully
on its own** -- whatever caused the earlier `NO_AP_FOUND`/`AUTH_EXPIRE`
flapping apparently wasn't a persistent incompatibility (or self-
resolved). Verified the `WiFi.begin(ssid, pass, channel, bssid)` 4-arg
overload and `WiFi.channel()`/`WiFi.BSSID()` signatures against the real
`espressif/arduino-esp32` source after the fact -- they're correct, so
the exact crash mechanism inside the join path is still unconfirmed; the
bootloop evidence (every boot, no PlatformIO here to repro) was reason
enough to not re-risk it blind a second time.

**Reverted `TimeSync.cpp`'s join call back to the plain two-arg
`WiFi.begin()`** (the confirmed-working version) -- dropped the channel/
BSSID targeting entirely. Kept `WiFi.setSleep(false)` (a simple no-
pointer-args standalone call, much lower risk, and a real documented
fix for WiFi auth flakiness on its own) in case it still helps, since
plain `begin()` was already proven safe with it absent at c8a65e1 and
setSleep() doesn't change the join call's own signature/risk profile.

**Added a real, generically useful fix alongside the revert**: TimeSync
had NO boot-crash guard at all, unlike Bluetooth's (`Persist::
markBtAttemptStarting/Done`, see "Settings + Bluetooth-on persistence"
below) -- and TimeSync runs on EVERY boot unconditionally (not gated
behind a persisted on/off flag the way BT's auto-resume is), so ANY
crash in its join path, this one or a future one, had zero recovery
path and would repeat forever. `Persist.{h,cpp}` gained
`markTimeSyncAttemptStarting()`/`markTimeSyncAttemptDone()` (same
pattern, new NVS key `tsPending`) wrapped directly around the actual
`WiFi.begin()`/connect-wait in `TimeSync::tryOnce()`; `Persist::load()`
checks `tsPending` on boot and sets `state.timeSyncSkipFirstAttempt`
(new `AppState.h` field) if the previous attempt never confirmed
completion, which `tryOnce()` consumes once to skip just that boot's
first attempt instead of repeating a crash. This is a real safety net
regardless of whether this exact join-path bug recurs.

**Not yet hardware-confirmed**. Next real step: flash, confirm WiFi
sync still works (should be unchanged from the known-good c8a65e1
behavior) and no bootloop.

## Twenty-fifth real hardware bug (found, fixed): scrubbing visually "rubberbanded" back during an active scrub -- the real-position sync was fighting it

User correctly self-diagnosed this one: "the sync of the bar is fighting
the scrubbing." Confirmed exactly right by reading both sides. `UI.cpp`'s
`tickPlaybackClock()` runs every ~500ms and unconditionally syncs
`state.now.posSec` from `AudioBridge::currentTimeSec()` (the REAL
decoder position) whenever a real file is loaded -- with zero awareness
of `InputRouter.cpp`'s debounced scrub-commit (`scrubSeekPending`,
`kScrubIdleCommitMs=400`). While actively scrubbing, `rotate()` updates
`state.now.posSec` locally every detent, but the REAL seek doesn't fire
until rotation goes idle for 400ms -- so every ~500ms, the unconditional
sync overwrote that local scrub position with the stale real one,
producing exactly the described "scrub moves the bar, then it snaps
back" loop, repeating the whole time the user kept turning (never idle
long enough to let the real seek commit and catch the display up).

**Fixed**: `AppState.h` gained `state.scrubPending`, set by `rotate()`
the moment a scrub starts (alongside the existing `scrubSeekPending`)
and cleared by `InputRouter::update()` right after the debounced real
seek actually commits. `tickPlaybackClock()` now skips the real-position
sync entirely while `state.scrubPending` is true -- the bar only moves
with the scrub itself during scrubbing, then picks up the real decoder
position again the instant the seek commits, matching exactly what was
asked for ("the song keeps playing, the bar only visually moves with
the scrubbing, then when I stop the sync picks up that new location").

**Residual, not addressed this round**: the user also reported real
audio glitches/static at the moment a long scrub finally commits, not
just a brief silent gap. This is a SEPARATE issue from the rubberbanding
(which was purely a display bug, now fixed) -- it's about the actual
seek/resync mechanism itself, likely the same territory as the
nineteenth bug's parked "scrubbing is laggy" finding (the CRC-8 fix
already shipped prevents crashes/freezes here, confirmed on hardware,
but doesn't claim to make every resync glitch-free). Not investigated
further this round -- needs confirmation after the rubberbanding fix
lands, since fixing the visual fight might also change how long/far a
real scrub gesture tends to run before committing, which could itself
affect how noticeable the residual glitch is.

## Twenty-sixth real hardware bug (reported, diagnostics added, not yet root-caused): device crashes/reboots turning Bluetooth off during an active reconnect loop

User reported a real crash-reboot while trying to turn Bluetooth off
specifically while it was "constantly trying to scan and connect" --
happened running on battery, untethered from a computer, so no serial
log exists for this one yet. Given this project's established heap-
exhaustion history (seventh/eighth/twentieth bugs -- Bluedroid's own
resident footprint permanently drops free internal heap to a ~39-40KB
baseline once initialized), the user's own "memory thing" guess is
plausible: `BluetoothSource::end()`'s teardown (`a2dpSource.end()` --
`disconnect()`, AVRC deinit, its own NVS writes) does real heap
allocation internally, same class of risk `begin()`'s already-guarded
`start()` call has, but `end()` itself has never had a heap guard or
even diagnostic logging.

**Not fixed blind** -- guessing at which specific internal call inside
the library's teardown path crashed, with no log to confirm against,
would be exactly the kind of guess this project avoids. Added the same
diagnostic logging `begin()` already has (free internal heap right
before the risky call) to `end()` instead, so the NEXT crash -- ideally
reproduced with a serial monitor attached -- gives real numbers instead
of needing another guess. If it turns out to be heap exhaustion,
`end()` would need a `radioHeapOk()`-style guard added mirroring
`begin()`'s -- not done yet since that's a real behavior change
(refusing to let the user turn Bluetooth off) that shouldn't be guessed
at without confirming the cause first, unlike `begin()` where skipping
is clearly the safe default.

**Next real step**: reproduce with `pio device monitor` attached (even
briefly, over USB, not on battery) and capture the actual panic output
plus the new heap log line right before it.

**UPDATE -- ROOT CAUSE FOUND, real crash log obtained, fixed.** User got
a monitor attached and caught the exact crash -- this wasn't a heap
issue at all, it was `TimeSync.cpp`'s `WiFi.setSleep(false)` (added in
the twenty-fourth bug's "fix", kept through the revert since it was
judged low-risk): the log showed, in full:
```
E wifi:Error! Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled!!!!!!
abort() was called at PC 0x4022b264 on core 0
```
This is a real, documented ESP-IDF requirement: when classic Bluetooth
and WiFi are both active on the shared radio (this app's normal
operating state -- BT commonly stays connected/reconnecting while
TimeSync's periodic sync runs), WiFi modem sleep MUST stay enabled for
the coexistence arbiter to work; disabling it hard-aborts the whole
device the moment both radios are live at once. The user's crash
("trying to turn Bluetooth off" while BT was mid-reconnect-loop) lined
up exactly: TimeSync's periodic scan ran concurrently, hit
`WiFi.setSleep(false)`, aborted. **"Low risk because it's a simple
no-pointer-args call" was the wrong lens -- simplicity of the call
signature says nothing about whether it's safe to call, and this is
exactly the kind of IDF-level behavioral requirement that needs to
actually be known, not inferred from how simple an API looks.** Removed
`WiFi.setSleep(false)` entirely from `TimeSync.cpp` -- never reattempt
this without independently confirming IDF's coexistence requirements
first, regardless of how it's framed as a "fix" for something else.

This is almost certainly ALSO the real cause of the twenty-fourth bug's
original bootloop, not the BSSID/channel `WiFi.begin()` overload that
got blamed and reverted at the time (that revert was harmless to make
regardless, and the overload itself was independently verified correct
against the real header -- but this `setSleep(false)` line was present
in that same commit and is now proven, not just suspected, to cause
exactly this class of crash under real operating conditions).

## Twenty-seventh real hardware bug/decision: Bluetooth no longer auto-resumes on boot -- user's explicit call after living through the crash loop above

Same crash above exposed a real design problem independent of the
`setSleep` bug itself: `state.btOn` persisting across reboots meant
Bluetooth auto-resumed on EVERY boot, immediately starting an unattended
reconnect-then-discovery loop the moment the device powered on -- a
real, continuous radio/battery cost, and (while the device was bonded
to headphones that could be powered off at any time, same real session)
a real crash surface combined with TimeSync's periodic WiFi scans
sharing the same radio. User's own framing: "bluetooth should be off by
default... I should turn on Bluetooth manually so it's not constantly
trying to scan and connect," with "keep remembering on/off state but
stop hammering reconnect forever" offered as a fallback.

**Implemented the user's actual stated preference, not just the
fallback**: `main.cpp`'s `setup()` no longer calls `BluetoothSource::
begin()` based on persisted `state.btOn` at all -- removed the whole
boot-auto-resume block. `state.btOn`/`Persist::save()`/`load()` still
exist and still drive the Bluetooth screen's remembered on/off display
and the shadow-reconnect address (still used so a MANUAL "Bluetooth On"
reconnects without needing pairing mode again, see the twenty-first
bug) -- just no longer acted on automatically at boot. Turning Bluetooth
on is now always an explicit user action from the Bluetooth menu.
`Persist::markBtAttemptStarting()`/`markBtAttemptDone()` (the boot-
crash guard from the seventh bug) are now dead code for this call site
-- left in place rather than deleted, harmless, and the manual "Bluetooth
On" row never needed them anyway (already safe by ordering, per
Persist.h's existing comment).

**Also added, addressing "stop hammering reconnect forever" for when
Bluetooth IS manually turned on and nothing's in range**:
`BluetoothSource::tick()` (new, called once per `loop()` iteration) --
a simple application-level watchdog, since the library itself exposes
no "give up after N attempts" knob reachable from app code for its
reconnect-then-fallback-to-discovery sequence (confirmed by reading
`handle_reconnect_logic()`/the GAP discovery-failed handler again --
both effectively retry forever via the library's own 10s heartbeat once
something is actively searching, with no public way to bound that from
outside). Tracks how long BT has been disconnected-and-searching
(reset to "not searching" the moment a real connection lands, so a
LATER drop after hours of good connection gets its own fresh window,
not one measured from the original `begin()` call); past 60 seconds of
fruitless searching, calls `end()` itself. `main.cpp`'s existing
`syncBluetoothToUi()` already picks this up automatically next loop
(it already syncs `state.btOn` from `BluetoothSource::isRunning()`
every iteration) -- no new UI-side wiring needed.

**Not a new "stop searching" button** -- checked `MenuEngine::
enterBluetooth()`'s screen first: the existing "Turn Bluetooth Off" row
is unconditionally present (not gated behind connection state) and
already calls `BluetoothSource::end()` unconditionally, so it already
IS a universal stop-everything control; the crash above is what made it
LOOK broken/missing, not an actual gap. "Choose device..." (the real
discovery picker, already built -- see the dedicated section above) is
already the explicit manual-scan trigger the user also asked for.
Nothing new needed on either front.

**Not yet hardware-confirmed** (the no-auto-resume and give-up-watchdog
changes specifically -- the `setSleep` removal above IS confirmed, from
the real crash log). Next real step: flash, confirm Bluetooth stays off
after a power cycle until manually turned on, and confirm turning it on
with nothing in range gives up on its own after about a minute instead
of continuing to search.

## Real Bluetooth audio -- DONE, not yet hardware-confirmed

Was initially scoped out of an earlier round of this session (see the
open questions this section used to list) rather than attempted blind.
Came back to it after properly reading `ESP32-audioI2S`'s real decode
path and found it didn't need any guessing after all: `Audio.h` already
declares a documented weak-symbol extension point --
```cpp
extern __attribute__((weak)) void audio_process_i2s(int16_t* outBuff,
    uint16_t validSamples, uint8_t bitsPerSample, uint8_t channels,
    bool *continueI2S); // record audiodata or send via BT
```
-- called by `Audio::playChunk()` with every decoded PCM buffer right
before it would go to I2S, AFTER the library's own mono-upmix and
8-bit-to-16-bit widening, so it's always already 44.1kHz 16-bit stereo
interleaved -- exactly the format A2DP wants, answering all three of
the open questions from before (hook point: yes, this; additive: yes,
`*continueI2S=false` skips the I2S write cleanly; format: already
matches, no conversion needed). This answers "no shared hook point"
from the earlier scoping pass -- that was wrong, or at least premature;
the hook was there, just not found until actually reading the decode
path end to end instead of stopping at the public header.

**Implementation**: `AudioBridge.cpp` defines `audio_process_i2s()` at
global scope (the weak symbol isn't inside any namespace, confirmed
from the real header) -- whenever `BluetoothSource::isConnected()`,
feeds the exact PCM buffer into a new `BluetoothSource::feedPcm()` and
sets `continueI2S=false`; otherwise unchanged normal wired I2S output.
`BluetoothSource.cpp` gained a small ring buffer (`kPcmRingSize`,
16KB, PSRAM-allocated per this project's established PSRAM push) --
`feedPcm()` (producer, called from the main/audio task) and a new
`providePcm()` pull callback (consumer, called from the BT stack's own
task, registered via `set_data_callback()` in place of the old
`provideTestTone()`) run on different FreeRTOS tasks, synchronized by a
plain mutex (`xSemaphoreCreateMutex`). Underruns (not enough decoded
audio buffered yet) fill with silence rather than garbage -- a brief
gap reads better than noise. The old 440Hz test-tone generator is
fully removed, not just unused.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox to
compile against, same caveat as every change this session. Next real
step: flash, connect Bluetooth, play a real track, confirm actual music
comes out instead of the test tone -- and separately confirm wired
(non-BT) playback is unaffected (the `continueI2S=true` branch should
be byte-for-byte the same behavior as before this change).

**UPDATE -- hardware-confirmed partially working, with two real
follow-up bugs, both now fixed.** User confirmed real audio DOES come
out over Bluetooth now (no more test tone) -- but reported it as
"laggy and noisy and glitchy and overall just unusable."

**Root cause**: `feedPcm()` dropped the OLDEST buffered audio on
overflow instead of exerting any backpressure on the producer.
Skipping the I2S write (`continueI2S=false`) removes the one thing
that was naturally pacing the decode loop to real-time -- a blocking
I2S write takes roughly as long as the audio it writes actually takes
to play. Without that pacing, nothing stopped `audio.loop()`'s decode
from producing chunks faster than A2DP could drain them, and "drop
oldest" was silently throwing away chunks of audio mid-stream -- which
is exactly what a glitchy, discontinuous stream sounds like. The 16KB
(~93ms) buffer was also thin margin for real BT link jitter on top of
that.

**Fixed**: `kPcmRingSize` bumped to 65536 (~372ms headroom -- PSRAM is
abundant, 4MB, and a player doesn't care about a few hundred ms of
extra latency the way a real-time call would). `feedPcm()` rewritten to
block (in short bursts, bounded to 250ms total so a stalled/
disconnected consumer can't hang the main loop forever) for room
instead of dropping -- this re-creates the real-time pacing a blocking
I2S write used to provide, letting the decode loop naturally slow to
match what A2DP can actually drain, instead of racing ahead and
corrupting the stream.

**Separately, a real crash -- root cause found and fixed, not just
diagnosed.** User disconnected the headphones (powered them off) and,
while clickpod was (as designed at the time) auto-reconnecting, pressed
"Turn Bluetooth Off" -- crashed with `Guru Meditation Error: Core 0
panic'ed (LoadProhibited)`, `EXCVADDR: 0x0000000c` (a near-null pointer
dereference). The log leading up to it showed the real mechanism
directly: `handle_reconnect_logic(): Attempting auto-reconnect, retries
left: 1000` -> `esp_a2d_connect()` fired -> moments later our own
`end()` log line -> then two BT stack log lines visibly INTERLEAVED
character-by-character (`bt_app_av_state_co[n n8e5c5t7i6n]g[_Ih]d...`)
-- unambiguous evidence of two FreeRTOS tasks writing to Serial at the
literal same instant, i.e. genuinely concurrent execution, not just a
fast sequence.

Confirmed the exact mechanism by reading `BluetoothA2DPSource::end()`
in the real library source: it has `while(discovery_active) delay_ms
(100);` -- a real, working guard that waits out an in-flight DISCOVERY
scan before tearing down -- but **no equivalent guard for an in-flight
CONNECT attempt**. The library's own reconnect heartbeat
(`handle_reconnect_logic()`, running on `bt_app_task`, the library's
own dedicated event task) had just called `connect_to()`/
`esp_a2d_connect()` -- an async, in-flight profile-level operation --
at the exact moment our `end()` (running on the main/calling task)
started tearing down AVRC/GAP/A2D state out from under it. Two tasks
touching the same shared state with nothing serializing them is a
textbook crash, and this is a real, confirmed gap in the library's own
`end()`, not a heap issue (heap was low at 14524 bytes when this
happened, which may have compounded things, but the interleaved-log
evidence points squarely at the concurrency race as the actual trigger
-- heap exhaustion alone doesn't produce two tasks visibly writing to
UART at once).

**Fixed at the real root, not by avoiding calling `end()` carefully**:
`begin()` now calls `a2dpSource.set_auto_reconnect(allowAutoReconnect,
0)` -- the real library source confirms a second, public overload,
`set_auto_reconnect(bool, int max_retries)`, that `BluetoothA2DPSource`
exposes specifically for this (name-hides the common base class's
single-arg version, so the existing call site picks up the new
behavior with no other code changes needed). With `max_retries=0`,
`handle_reconnect_logic()`'s very first post-disconnect heartbeat check
takes its OTHER branch -- a one-shot discovery scan, not `connect_to()`
-- which `end()` DOES safely wait out via the existing
`discovery_active` guard. This doesn't just make the crash less likely,
it removes the specific unsafe state (an in-flight `connect_to()`) that
`end()` can ever be called into.

**Also directly implements the user's explicit UX ask** ("once a
device is disconnected, turn off and stop looking for that device"):
`BluetoothSource::tick()` (already polling every `loop()` iteration)
now tracks `hasEverConnectedThisSession` -- the moment a REAL
connection (not just the first attempt) drops, it calls `end()`
immediately, not after the 60-second give-up window from the
twenty-seventh bug (that window still applies, unchanged, to the
*first* connection attempt of a `begin()` session, where the device
simply not being in range yet is an ordinary case worth a real chance,
not a disconnect). This is a stronger, simpler, and SAFER design than
the previous 60-second watchdog for the post-connection case: reacting
on literally the first tick after a drop means `end()` runs before the
(now-neutered, but still real) heartbeat has any chance to reach its
risky discovery-scan fallback, rather than coexisting with it for up to
a minute.

**Not yet hardware-confirmed**, same caveat as every change this
session. Next real step: flash, play real audio over Bluetooth and
confirm it's actually clean now (not glitchy/noisy), then specifically
reproduce the original crash scenario (connect, power off the
headphones, wait for the "device disconnected -- turning Bluetooth
off" log line to confirm the new immediate-off behavior, then separately
confirm pressing "Turn Bluetooth Off" at any point, including
immediately after a disconnect, no longer crashes).

## AOD ("keep playing + lock input + show clock") -- software half built, screen dimming still blocked on backlight hardware

Plan item 7's software-only half (everything that doesn't need the
parked backlight GPIO rewiring) is real now, built on the EXISTING
`AppMode::OFF`/`drawOff()` rather than a new mode -- a CENTER long-press
already toggled into/out of this mode; what was missing was making it
actually behave like a locked "asleep, still playing" screen instead of
a dead-feeling "powered off" one.

**Found and fixed, a real correctness gap**: `InputRouter.cpp`'s
`handleLongPress()` had `if (btn == AnoButton::CENTER) togglePower();`
first, then unconditionally `if (btn == AnoButton::RIGHT)
MenuEngine::enterBluetooth();` -- that second check ran regardless of
mode, including `AppMode::OFF`. So a RIGHT long-press while "locked"
(exactly the accidental-bag-press scenario AOD exists to prevent) would
silently wake Bluetooth and jump into its menu -- the single gap in what
was otherwise already-correct input gating (`handleTap()`/`rotate()`/
`handleDoubleTap()` all already checked `state.mode == AppMode::OFF`
and bailed). Fixed with one added guard right after the CENTER check:
every OTHER long-press, not just taps/rotation, now ignores input while
OFF.

**Found and fixed, a real functional gap**: `UI.cpp`'s
`tickPlaybackClock()` had `if (state.mode == AppMode::OFF) return;` as
its second line -- meaning position tracking, track-end auto-advance,
AND the ninth-hardware-bug's decode-failure auto-skip ALL silently
froze the entire time the screen was locked. "Keep playback running
exactly as-is" explicitly means queue advancement too, not just the
decoder continuing to physically produce sound on whatever track was
already loaded -- a track finishing while the screen was locked would
just stop, needing a wake+relock to notice and advance. Removed that
early return entirely; nothing in the rest of the function touches the
screen (it only updates `state.now.*` and sets dirty/progressDirty
flags that the OFF-mode render branches already correctly no-op on), so
there's no stray-redraw risk from removing it.

**`drawOff()` now actually shows what AOD promises**: gained a real,
live clock (`drawOffClock()`, reusing `TimeSync::currentTimeString()`,
the same real wall-clock the statusbar now shows -- see the
twenty-third bug above) and a "playing"/"paused" line reflecting
`state.now.playing` when a track is loaded, instead of a flat black
screen with only "hold CENTER to power on." `Screens::render()`'s
existing `statusbarDirty` lightweight-redraw path (added for the
twenty-third bug) now branches to `drawOffClock()` specifically for
`AppMode::OFF` instead of skipping it outright, so the locked screen's
clock actually advances once a minute instead of freezing at whatever
it showed the moment the screen locked -- same reasoning, same
mechanism, just a different draw target depending on mode.

**Still blocked, unchanged from before**: real screen DIMMING needs the
backlight off the 3.3V rail and onto a GPIO, which is parked after three
failed real-hardware strapping-pin attempts (see the "Next session
plan" item 7 writeup below for the full history) -- nothing attempted on
that front this round, it's a physical rewiring job for whenever the
user wants to pick it back up, not something fixable from here. The
locked screen is fully real/functional now (input locked, playback
keeps running and advancing, a live clock shows) -- it just isn't dim
yet, same brightness as any other screen until that hardware work
happens.

**Not yet hardware-confirmed** -- same caveat as every change this
session, no PlatformIO in this sandbox to compile against. Next real
step: flash, start playback, long-press CENTER to lock, confirm (a) the
clock appears and updates, (b) every button except a CENTER long-press
does nothing while locked (try RIGHT long-press specifically -- that's
the one that was broken), and (c) playback actually advances to the
next queued track if one finishes while still locked.

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

## Twenty-eighth real hardware bug (found, fixed, not yet hardware-confirmed): real root cause of "garbled and noisy" Bluetooth audio -- wrong byte-count math, not a buffering issue

The previous round's Bluetooth-audio fix (bigger ring buffer + blocking
backpressure in `feedPcm()`, see "Real Bluetooth audio" above) was a real
improvement but the user still reported it as unusable. Re-read
`Audio.cpp`'s own `playChunk()` (the real pinned 3.0.12 source, not
`Audio.h`'s public declaration this project usually checks against) to
find what else could explain "noisy/garbled" rather than a timing/dropout
symptom -- and found a genuine, structural bug in `AudioBridge.cpp`'s
`audio_process_i2s()` hook, present since the feature was first built:

`Audio.h`'s weak-symbol hook signature gives `validSamples` alongside
`bitsPerSample`/`channels`, but `audio_process_i2s()` discarded the
latter two with `(void)` casts and computed the byte count fed into
`BluetoothSource::feedPcm()` as `validSamples * 2` -- an assumption that
`validSamples` is already a total 16-bit-word count. Checking the real
i2s write right after this hook fires in `Audio.cpp` proves that's wrong:
```cpp
err = i2s_write(..., (int16_t*)m_outBuff + count,
                 validSamples * (sampleSize * m_channels), &i2s_bytesConsumed, 40);
```
`validSamples` is a **frame count** (one unit per interleaved sample
group across all channels), not a word count -- the real byte size is
`validSamples * (bitsPerSample/8) * channels`. For the normal 16-bit
stereo case that's `validSamples * 4`, not `validSamples * 2` -- the old
code was feeding BluetoothSource exactly HALF of every decoded PCM chunk
into the ring buffer, with the second half silently left behind (never
read, overwritten by the next decode pass). That's real, structural data
corruption -- cutting every chunk in half mid-stream and resuming the next
chunk from the wrong offset -- not a starvation/timing issue, which is
why the bigger-buffer-plus-backpressure fix from the previous round
(itself a real, worthwhile fix, kept as-is) couldn't have addressed it:
the data handed to the ring buffer was already wrong before it ever got
there.

**Fixed**: `audio_process_i2s()` now uses the real `bitsPerSample`/
`channels` parameters instead of discarding them --
`byteCount = validSamples * (bitsPerSample/8) * channels` -- matching
the library's own internal formula exactly rather than hardcoding an
assumption about the buffer's layout.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox. Next
real step: flash, connect Bluetooth, play real audio, confirm it's
actually clean now. If it's still not clean after this, the ring-buffer/
backpressure design from the previous round is the next thing to
re-examine (this fix and that one address different failure modes, both
real, and nothing rules out there being a third).

## Twenty-ninth real hardware bug/decision (found, fixed, not yet hardware-confirmed): Queue screen couldn't scroll back past "now" -- the whole history list existed but was never shown

User asked for free bidirectional navigation through the current
playlist/queue instead of the classic "Spotify can't scroll back past
where you started" behavior. Investigated `state.history` (tracked since
early in this project, used only by `skipPrevious()`'s one-step-back
case) and `Screens.cpp`'s `drawQueue()` (only ever rendered
`state.queue`, the upcoming-tracks list) -- confirmed the data needed for
free navigation already existed, it just was never surfaced: history was
tracked and kept correct the whole time, nothing ever showed it or let
you jump into it beyond one step.

**Fixed by combining history + the currently-playing track + the
upcoming queue into one continuous, freely-scrollable list** on the
Queue screen, instead of adding a separate screen or mode:

- `MenuEngine.cpp` gained `playFromHistoryIndex()` (generalizes
  `skipPrevious()`'s one-step logic to jump back arbitrarily far --
  everything between the picked history item and "now" moves onto the
  front of the queue, in order, so playing forward from there revisits
  exactly what got skipped over) and `playFromCombinedIndex()` (dispatches
  a combined-list index to history/now/queue). `trackAtCombinedIndex()`
  lets the "..." track menu (Play Next / Add to Queue / Add to Playlist)
  open from ANY row now, not just an upcoming-queue one.
  `queueSelectionIsQueueItem()` gates the RIGHT-tap grab/reorder gesture
  to upcoming-queue rows only -- dragging an already-played history row
  or the currently-playing track around has no meaning, so that gesture
  is a no-op there instead of attempting something nonsensical.
- `state.queueSelected` changed meaning: it now indexes the COMBINED
  `[history | now | queue]` space, not `state.queue` alone.
  `moveQueueSelection()`/`moveGrabbedQueueItem()` (the latter still
  confined to the queue segment, translating to/from a local
  `state.queue` index internally) were updated accordingly.
  `InputRouter.cpp`'s entry point (long-press UP from Now Playing) now
  anchors the cursor on the "now playing" row (`state.history.size()`)
  instead of row 0 of the upcoming queue, so opening the screen reads as
  "history above, queue below" rather than starting mid-list.
- `Screens.cpp`'s `drawQueue()` renders all three segments in one
  scrollable viewport: history rows (muted text, counting backward from
  -1 at the track just before now), a "now playing" row (a small filled
  triangle glyph instead of an index number -- the anchor the rest of the
  list scrolls around, needs to read as structurally different from a
  plain numbered row), then upcoming-queue rows (unchanged from before,
  counting forward from 1).

**Not simulator-ported, not spec-amended this round** -- same honest
gap this file already flags for several other recent real-hardware fixes
(row icons, the various BT/heap rounds): this was real behavior-changing
UX, which CLAUDE.md's usual discipline says should go through the
simulator first, but the user asked to move fast on a live bug/feature
request rather than a UX-iteration round. Worth a follow-up simulator
port + `docs/SPEC.md` amendment if this flow needs further iteration --
flagged, not silently skipped.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox. Next
real step: flash, play a few tracks so history accumulates, open the
Queue screen and confirm it shows past tracks above "now" and upcoming
ones below, scroll up and CENTER on a past track to confirm it actually
jumps back and requeues what was skipped, and confirm grab/reorder still
works for upcoming-queue rows but is a no-op on a history/now row.

## How wired vs. Bluetooth output switching actually works (traced from code, not yet real-hardware-tested for this specific scenario)

User asked, not yet having tested it: what happens if BT connects mid-
playback, or connects while wired is already "active," or the reverse.
Worth writing down precisely since the answer turned out to be simpler
than the question implies, once traced through the real code path.

**There is no "wired device" that connects/disconnects at all.** The
PCM5102A DAC is permanently wired to the I2S bus (`AudioBridge::begin()`
calls `audioPtr->setPinout(...)` once, at boot) -- there's no headphone-
jack-detect circuit anywhere in the BOM/`Pins.h`, so there's no runtime
"wired connected" event for firmware to react to in the first place.
Wired output is just always physically live, continuously fed whatever
`Audio::playChunk()` writes to it (or doesn't).

**Routing is decided fresh on EVERY decoded PCM chunk, not once per
track or once per connection event.** `AudioBridge.cpp`'s
`audio_process_i2s()` -- called by the library for every chunk right
before it would go to I2S -- checks `BluetoothSource::isConnected()`
live, every single call:
- `isConnected()` true -> chunk goes into `BluetoothSource::feedPcm()`'s
  ring buffer, `*continueI2S = false` (the wired I2S write for that
  chunk is skipped).
- `isConnected()` false -> `*continueI2S = true`, normal wired I2S write,
  completely unchanged from how it's always worked.

This means every scenario asked about resolves the same way, automatically,
with no special-cased "device connected" handling needed anywhere:
- **BT connects mid-playback** (user turns BT on while a track is already
  playing over wired): during the A2DP connection handshake,
  `isConnected()` is still false, so wired keeps playing uninterrupted --
  no gap waiting for BT to pair. The instant `isConnected()` flips true,
  the very next decoded chunk routes to BT instead and wired stops
  getting fresh data. This is a hard cut on a chunk boundary, not a
  crossfade -- see the open question below.
- **BT is already connected and wired "connects"**: doesn't apply, per
  above -- wired has no connect event, it's just a question of whether
  `isConnected()` is true or false at any given moment.
- **BT disconnects mid-playback** (headphones walk out of range/power
  off): the next chunk's `isConnected()` check goes false immediately,
  so routing falls back to wired on literally the next decoded chunk --
  independent of `BluetoothSource::tick()`'s own `end()` cleanup call
  (twenty-seventh bug/BT redesign section above), which only runs once
  per `loop()` iteration and exists to stop the library from trying to
  reconnect, not to restore audio output (that already happened by the
  time `tick()` gets to it).
- **Never double-outputs or drops both**: it's a strict either/or per
  chunk based on one live boolean check, so there's no window where both
  outputs are silent or both are live at once.

**What's NOT verified, and worth listening for specifically on the next
real test**: whether the hard cut at the exact moment of a BT connect/
disconnect produces an audible click/pop/glitch -- there's no fade or
synchronization around the switch, just `continueI2S` flipping between
one chunk and the next. Also not independently confirmed: what the I2S
peripheral itself does with a skipped write (whether the DMA buffer
just holds/repeats its last contents for that one cycle, which would be
inaudible at a single-chunk timescale, or something else) -- reasoned
from how `continueI2S` is used, not traced into the IDF I2S driver
itself. If a real test reveals an audible artifact at the switch point,
this is the first place to look, not a sign the routing logic itself is
wrong.

## Album art is routinely cropped awkwardly -- deferred, not fixed yet

User flagged: embedded cover art on the Now Playing screen is "slightly
too big for the display area... awkwardly cropped all the time (sometimes
it works out fine but rarely)". Traced the real cause in `AlbumArt.cpp`:
the fixed `kSize = 92` buffer (matching Screens.cpp's Now Playing art
box) is filled by picking the FINEST power-of-2 JPEG decode-time scale
(`TJpg_Decoder` only supports 1/2/4/8x, no arbitrary resize) that still
leaves the decoded image >= `kSize` on both axes, then center-CROPPING
that decoded image down to exactly 92x92 -- there's no "fit/contain"
path, only "decode to the smallest available size that's still big
enough, then crop off whatever doesn't fit." Since real embedded art is
almost never a clean multiple of 92 at one of the 4 available scale
factors, the decoded-but-pre-crop image is often meaningfully larger
than 92x92 (e.g. a 300x300 source decodes at scale=2 to 150x150, then
gets cropped by ~29px off every edge -- repeat this chunk size per file,
it extends/varies with the title's actual resolution), which matches
"rarely works out fine" -- it only looks right when a particular file's
resolution happens to downscale to something close to 92 already.

**UPDATE -- fixed, not yet hardware-confirmed.** Implemented option 1
from the two originally proposed: `AlbumArt.cpp`'s `tjpgCallback()`/
`loadForTrack()` now decode into a SEPARATE, per-track scratch buffer
(`decodeBuf`, sized to that file's actual post-TJpg-scale dimensions,
`ps_malloc()`'d and freed each load) instead of writing straight into
the fixed `kSize`x`kSize` `artBuf` with a centering offset -- that
offset-write WAS the crop (anything outside `[0,kSize)` after centering
was silently clipped). After decoding into the full scratch buffer, a
real nearest-neighbor resize step fits it into the `kSize`x`kSize` box
preserving aspect ratio (scales to whichever axis is smaller, letterbox-
pads the other with centering, same as the old crop's centering math but
now shrinking instead of clipping) -- no more thrown-away image content.
Nearest-neighbor, not a box/bilinear filter -- cheap, and the source is
already close to `kSize` after the existing TJpg scale-factor selection,
so a fancier filter wouldn't gain much at this size. Option 2 (growing
`kSize`/the art box itself) is NOT done -- the resize alone should fix
the "awkward crop" complaint; growing the box is a separate, bigger
layout change (touches `Screens.cpp`'s Now Playing layout, not just
`AlbumArt.cpp`) and wasn't asked for alongside this fix. Revisit only if
real art still looks too small/soft after this round confirms on
hardware.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox. Next
real step: flash, play a few tracks with real embedded art (especially
ones that looked badly cropped before) and confirm the full image now
shows, just possibly letterboxed (thin padding bars on one axis for
non-square art), not cut off.

## Thirtieth real hardware bug (found, fixed, not yet hardware-confirmed): the 128-byte PSRAM threshold wasn't actually catching real Track strings -- BT stalled mid-session, then refused to connect at all after a fresh RST

Testing the twenty-eighth/twenty-ninth bugs' fixes (BT byte-count, queue
navigation) while playing from the full 425-track "funky times" playlist
surfaced two connected failures:

1. **Mid-session**: BT audio went sluggish, the ring buffer logged
   "stayed full too long -- dropping the rest of this chunk" continuously,
   and no sound came out at all. User noted this happened while a wired
   jack was also plugged in -- unplugging it did NOT fix it, only a full
   RST did. The jack correlation is most likely coincidental, not
   causal: this board has no jack-detect circuit (see the dedicated
   section above on wired/BT routing -- there's no "plugged in" signal
   firmware can even see), and "only a fresh boot fixes it" is a much
   stronger signature of the ring buffer/BT stack getting wedged in a bad
   state than of anything jack-related.
2. **After the RST**: Bluetooth refused to connect AT ALL, every attempt
   immediately logging `radioHeapOk()`'s skip message -- free internal
   heap stuck around 39.6-39.7KB against the 60KB cold-init floor
   (`kMinInternalHeapForRadioColdInit`, twentieth bug), with NOTHING
   about this heap number changing across repeated "Bluetooth on" taps.

**Real root cause, not a new bug -- the eighth hardware bug's prediction
confirmed with real numbers**: that writeup already flagged `state.queue`
holding every remaining `Track` (artist/album/title/path `String`s) of
whatever's actually playing, for the WHOLE playback session, as the
likely next structural heap problem once scanning itself stopped being
the bottleneck -- and said the fix, if needed, would be moving that
String data to PSRAM. The twentieth bug's fix (lowering
`heap_caps_malloc_extmem_enable()`'s threshold 4096 -> 128) was aimed at
exactly this, but the real number proves 128 wasn't low enough: a real
full nested path from this exact test session --
`/funky times/Dire Straits/Making Movies/Dire Straits - Tunnel of Love
(Intro The Carousel Waltz).flac` -- is only 102 bytes, UNDER the 128-byte
floor, so even a long real path (let alone a short artist/album name)
was still landing in internal RAM, not PSRAM. Playing a 425-track
playlist keeps roughly that many Track structs (up to 4 strings each)
resident in `state.queue` continuously -- this is very plausibly tens of
KB of internal RAM that should have been living in PSRAM the whole time,
directly explaining both symptoms: heap pressure degrading BT/scheduling
enough mid-session to starve the data-callback consumer (ring buffer
fills, "stayed full" spam, no sound), and the same pressure persisting
across a reboot (the queue is rebuilt fresh each boot from the index,
same string-heavy shape) to permanently block the cold-init floor.

**Fixed**: `main.cpp`'s `heap_caps_malloc_extmem_enable()` threshold
lowered again, 128 -> 1 -- essentially any non-empty String allocation
(Arduino's String has no small-string optimization, confirmed in this
file's own earlier writeup) now prefers PSRAM, not just the rare large
one. Same safety argument as both previous lowerings (4096->128,
128->1): DMA-capable allocations explicitly request
`MALLOC_CAP_DMA`/`INTERNAL` and bypass this threshold regardless, so
WiFi/BT/I2S's own buffers are unaffected -- the only real cost is
PSRAM's marginally slower random access vs. internal RAM, negligible for
text data that's compared/copied occasionally, not hot-looped.

**Deliberately did NOT touch `kMinInternalHeapForRadioColdInit` (60KB)
itself** -- lowering the actual safety floor to match a low heap number
would risk reintroducing the hard-abort crashes that floor exists to
prevent (seventh/eighth bugs), with no confirmation cold-init is safe
at ~40KB. The intended fix is raising the real available heap back
above the existing floor, not lowering the floor to meet a degraded
heap number.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox. Next
real step: flash, load up the full "funky times" playlist again (play
from it so `state.queue` holds the same several-hundred-Track shape that
triggered this), and confirm (a) Bluetooth now connects on a fresh RST
instead of being stuck skipping at ~40KB, and (b) playing real audio
over BT during that same large-playlist session stays clean instead of
degrading into the "ring buffer stayed full" stall. If BT still can't
clear the cold floor after this, the next real step is getting a fresh
boot log's heap number specifically right before the first BT attempt,
to see how much (if any) this actually recovered -- if the number is
still short, the remaining gap is likely the Track VECTOR's own capacity
overhead (std::vector's heap-allocated backing array itself, one
allocation per vector, separate from each String's own buffer) rather
than the String buffers this fix targets, which would need its own,
separate PSRAM-allocator treatment.

## Thirty-first real hardware bug (found, fixed): queue "free navigation" only reached session-played history, not the rest of the source list you started in the middle of

User confirmed the twenty-ninth bug's combined Queue screen DOES work for
what it was built for -- scrolling back reaches the first track you
picked and everything played forward from it since. But picking track 5
of a 12-track album left tracks 1-4 completely unreachable: "still can't
go back a song after picking one in the middle... only forwards." Root
cause in `MenuEngine.cpp`'s `playQueueFrom()`: it only ever populated
`state.queue` with what comes AFTER the picked index
(`list.assign(list.begin()+index+1, list.end())`) and unconditionally
cleared `state.history` to empty -- everything before the picked index
in the source list was simply discarded at the moment playback started,
never tracked anywhere. The combined-list Queue screen (twenty-ninth
bug) was only ever showing ACTUAL session playback history, not "the
rest of the list you started from," which is what "free roam the
queue/playlist" actually means.

**Fixed**: `playQueueFrom()` now does
`state.history.assign(list.begin(), list.begin() + index)` instead of
clearing history -- the queue/history split now treats the whole source
list (whatever album/playlist/queue-row you picked from) as the
navigable context from the start: everything before the picked track
goes into history (in original order), everything after goes into the
queue, exactly symmetric. Starting playback from a specific track now
means "I'm positioned in the middle of this list," not "history begins
empty from here" -- scrolling up in the combined Queue screen reaches
the untouched earlier tracks immediately, no need to have ever played
them first. Picking a different list to play from (a different
album/playlist/queue-row) still replaces the whole context, same as
before -- each `playQueueFrom()` call establishes a fresh "list you're
positioned inside," discarding whatever context was active before it.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox. Next
real step: flash, pick a track from the middle of an album, open the
Queue screen, and confirm scrolling up shows the earlier untouched
tracks (not just empty/nothing above "now" until you've actually played
back to that point).

## Thirty-second real hardware bug (reasoned hypothesis, not confirmed root cause): new `spinlock_acquire` crash after scrubbing while Bluetooth streams real audio

User reported scrubbing through a track while connected over Bluetooth
worked for listening to different parts, but scrubbing BACK afterward
"lagged and crashed" -- `assert failed: spinlock_acquire spinlock.h:122`,
a multi-core heap-lock assert, immediately following a FLAC decode error
(`RESERVED CHANNEL ASSIGNMENT`) and a second resync (`stream ready`
twice in the log) right around the time of the scrub-back action.

**Not a confirmed root cause** -- this sandbox has no PlatformIO to
build against and no ELF to resolve the crash's raw backtrace addresses
into function names, so this is a reasoned hypothesis from the failure
signature and timing, not something traced to an exact line the way
most of this file's other fixes are. Flagging the distinction honestly
rather than presenting a guess as confirmed.

**The reasoning**: a `spinlock_acquire` assert is the classic signature
of memory corruption near a task's stack (ESP-IDF's heap allocator uses
a spinlock for cross-core safety; corrupted/overrun stack memory
adjacent to heap structures is a well-known way to trip this assert in
code that itself did nothing wrong). The crash followed a call-depth
combination that has never actually been exercised together before on
real hardware: a real seek (scrubbing back) triggers
`FLACDecoderReset()` and a resync, which happens deep inside the SAME
`loop()`-task call chain that also runs `Audio::playChunk()` ->
`audio_process_i2s()` -> `BluetoothSource::feedPcm()`'s bounded blocking
retry loop (new this session, the twenty-eighth bug's BT-audio fix) --
all of this nested inside whatever else `loop()` is doing (menu
rendering, SD/FlacMeta reads, Library lookups). Every previous seek/
resync crash (fourteenth/sixteenth/nineteenth bugs) was tested and fixed
against WIRED-only output, a simple synchronous I2S write -- this is
the first time a real seek has been exercised together with the BT
ring-buffer feed path's added call depth. The Arduino-ESP32 core's
default `loop()` task stack is a plain 8192 bytes (confirmed by reading
the real core source, `cores/esp32/main.cpp`) -- never sized against
this specific worst-case depth.

**Fixed (as a reasoned mitigation, not a proven fix)**: `main.cpp`
gained `getArduinoLoopTaskStackSize()` returning 16384 instead of the
default 8192 -- a real, documented override point (declared
`__attribute__((weak))` in the core specifically so a sketch can resize
it), confirmed from source, not guessed. Deliberately a moderate +8KB,
not a large jump -- mindful of this project's hard-won internal-heap
headroom (the thirtieth hardware bug's PSRAM-threshold work); stack is
a one-time static allocation at boot, not an ongoing cost the way BT/
WiFi's dynamic heap pressure is, so this is a bounded, known tradeoff.

**If this doesn't fix it**: the next real step is getting a SYMBOLIZED
backtrace (needs the actual built `.pio/build/esp32wrover/firmware.elf`
and `xtensa-esp32-elf-addr2line`, or `pio run -t ... | ... monitor`'s
own symbolication if the platform IDE does it automatically) rather than
guessing at a second hypothesis blind -- the raw hex backtrace in the
report isn't resolvable without the actual binary, which doesn't exist
in this sandbox. Worth also trying to reproduce on a file that does NOT
trigger a mid-playback decode error, to see whether the decode-error/
resync is actually necessary to trigger this or just coincidental timing.

**Follow-up investigation, strengthening confidence this is the right
side to fix**: cloned the real ESP-IDF Bluedroid source
(`components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c`)
to trace exactly what task calls our `providePcm()` data callback, to
rule in/out the CONSUMER side (the BT stack's own task, not ours to
resize via `getArduinoLoopTaskStackSize()`) as the real culprit instead
of the producer side. Confirmed: a periodic `osi_alarm` fires every
`BTC_MEDIA_TIME_TICK_MS`, which posts an event to a dedicated OSI thread
(`btc_aa_src_task_hdl`) rather than running inline in the timer/alarm's
own context -- so our callback runs on a real, ordinary FreeRTOS task,
not an ISR or timer-callback context where blocking would be outright
illegal. More importantly: `providePcm()` itself is lightweight on that
task's stack -- one bounded mutex take, a tight byte-copy loop, one
give, no recursion, no large local buffers -- so it's an unlikely
candidate for overflowing ITS task's own (separately-sized, IDF-
controlled, not ours to resize from the sketch) stack. The actual
complex, deep, genuinely-blocking work (FLAC resync/CRC-8 verification,
SD reads, `feedPcm()`'s up-to-250ms retry loop) all happens on the
PRODUCER side -- the main `loop()` task, which `getArduinoLoopTaskStackSize()`
DOES control. This doesn't prove the stack-size fix is correct, but it
does rule out the one alternative (consumer-task stack) that would have
made the fix target the wrong side entirely, so it's the right first
thing to try.

## Thirty-third real hardware bug (found, fixed, not yet hardware-confirmed): two independent, unsynced volume controls stacking over Bluetooth

User reported the headphones' own volume buttons genuinely changed the
audible level (visible in the serial log too) while the on-screen
slider behaved as a completely separate control -- "theoretically I can
blast the volume on both... or reduce it to zero on both but one would
be undetectable." Separately: the on-screen slider felt coarser than
the headphones' own buttons, and could be pushed louder than the
headphones' own controls could reach on their own ("headphone ones tap
out at 127 but I can push it further maxing out the on-screen ones").

**Traced the real cause by reading both the real `ESP32-audioI2S` and
`ESP32-A2DP` sources directly**: `Audio::playChunk()` applies its own
`Gain()` (the volume `AudioBridge::setVolumePercent()` sets, 0-21 range)
BEFORE calling `audio_process_i2s()` -- confirmed from source, not
guessed -- meaning whatever reaches Bluetooth is ALREADY attenuated by
that value. Separately, `BluetoothA2DPCommon.h`'s `set_volume()` (0-127)
attenuates AGAIN via its own internal `volume_control()`, and the
library calls this AUTOMATICALLY AND UNCONDITIONALLY every time the
connected device reports ITS OWN volume changed (confirmed from
`BluetoothA2DPSource::bt_av_notify_evt_handler()`'s
`ESP_AVRC_RN_VOLUME_CHANGE` case) -- not something app code can suppress
or opt out of. Two independent attenuation stages, each able to move
on its own, exactly matches what was reported. The "127 vs. can push
further" complaint is the same root cause from a different angle: the
on-screen slider only ever drove the 0-21 DAC-range stage, with the
127-range AVRCP stage able to independently pile more or less
attenuation on top regardless of what the slider showed.

**Fixed**: `AudioBridge::setVolumePercent()` now makes only ONE stage
active at a time instead of letting them fight -- while Bluetooth is
connected, it pins the wired/I2S side at max (21, unity -- no
attenuation before `audio_process_i2s()` sees the samples) and drives
the real, single volume control through a new `BluetoothSource::
setVolume()`/`getVolume()` pair (wrapping the library's real
`set_volume()`/`get_volume()`) scaled to its native 0-127 range instead
-- which also directly fixes the coarseness complaint, since 127 steps
is finer than the DAC path's 21. `BluetoothSource::setVolume()` both
attenuates OUR output and sends the AVRCP "set absolute volume" command
so the connected device's own volume readout updates to match (real,
built-in library behavior, confirmed from source, not something this
project had to implement itself). The OTHER direction -- the headphones
reporting their own volume change -- was already being applied to real
attenuation automatically by the library (per the unconditional-call
finding above); what was missing was telling OUR UI about it.
`main.cpp`'s `syncBluetoothToUi()` now polls `BluetoothSource::
getVolume()` each loop tick while connected and mirrors a change into
`state.volume` (marking the UI dirty), and re-invokes
`AudioBridge::setVolumePercent(state.volume)` on every real connect/
disconnect transition so the right attenuation stage is always the one
actually live for the current output path. Wired-only playback (BT not
connected) is completely unchanged from before this fix.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox. Next
real step: flash, connect Bluetooth, confirm (a) the on-screen slider
and the headphones' own volume display move together no matter which
side initiates the change, (b) the slider can't be pushed past what the
headphones themselves can reach, and (c) the volume change actually
feels finer-grained than before.

## Thirty-fourth real hardware bug/decision (found, fixed, not yet hardware-confirmed): WiFi sync retrying every 2 minutes for hours while Bluetooth stayed on

User reported WiFi trying to sync "a lot more frequently than expected"
-- a power-saving concern, not a correctness bug. Root cause in
`TimeSync.cpp`: `kSkippedRetryDelayMs` (2 minutes) was designed, early
in this project, for a one-off TRANSIENT skip right at boot (low
internal heap immediately after the library scan) -- back before real,
long-duration Bluetooth usage was common. `RadioLock.h`'s design means
EVERY `TimeSync::tryOnce()` attempt gets skipped for as long as
Bluetooth is connected (the radio lock isn't available), and a real BT
listening session can run for hours -- meaning this fast-retry path was
firing a real WiFi scan attempt every 2 minutes, continuously, for the
entire time Bluetooth happened to be on, which is now the common case,
not a rare transient one.

**Fixed**: `kSkippedRetryDelayMs` raised from 2 minutes to 30 minutes --
still recovers much faster than the full 6h normal resync cycle from a
genuinely transient low-heap moment at boot, but doesn't hammer a WiFi
scan every couple of minutes for a multi-hour BT session. The normal
`kResyncIntervalMs` (6h, for an attempt that actually ran and failed for
an ordinary reason) is unchanged -- that one was never the issue.

**Not yet hardware-confirmed** -- no PlatformIO in this sandbox. Next
real step: flash, use Bluetooth for an extended session, and confirm
WiFi scan attempts in the serial log are now spaced out (roughly every
30 minutes while BT is on, not every 2).

## Deferred: full headphone remote-control compatibility (transport controls + ULT WEAR-specific features)

User flagged, not yet scoped or started: beyond volume (thirty-third bug
above), the headphones' own PHYSICAL controls should ideally also
reach play/pause and skip-forward/skip-back, and separately asked
whether device-specific features (the ULT WEAR's bass-boost EQ, ANC,
and transparency/ambient mode) can be made to work from this firmware,
or whether those are inherently headphone-side-only regardless of what
the source device does.

**Not investigated yet** -- noting what's known/suspected without
having verified any of it against real library source yet (unlike the
volume work above, which WAS verified before being called fixed):

- **Transport controls (play/pause/skip) are the standard AVRCP
  "passthrough" command set** -- this is the same protocol family the
  volume sync work above already confirmed this library implements
  (`BluetoothA2DPSource`'s AVRC Controller role). Headphones sending
  play/pause/next/previous button presses to a SOURCE device over AVRCP
  passthrough is extremely standard, common behavior (this is exactly
  how a phone receives headphone button presses) -- plausible this
  library already receives these events the same unconditional way it
  already handles `ESP_AVRC_RN_VOLUME_CHANGE`, just not yet traced into
  the source to confirm the exact event/callback names, or wired up to
  call `MenuEngine`'s existing play/pause/skip actions
  (`AudioBridge::pauseResume()`, `MenuEngine::playNextInQueue()`,
  `MenuEngine::skipPrevious()` already exist and do the right thing --
  this would likely be "receive the AVRCP event, call the function
  that already exists," not new playback logic).
- **EQ/bass-boost, ANC, transparency mode are almost certainly
  headphone-side-only, NOT controllable from a generic A2DP source** --
  this is a strong prior, not yet confirmed by reading source: standard
  Bluetooth A2DP/AVRCP profiles have no concept of "enable ANC" or "set
  bass boost" -- those are real-time DSP decisions the HEADPHONES make
  on their own hardware, usually from their own physical buttons or a
  companion phone app talking to the headphones over a proprietary
  vendor protocol (Sony's own app protocol for the ULT WEAR, in this
  case) that has nothing to do with A2DP/AVRCP and nothing this
  ESP32-A2DP-based firmware could plausibly speak without reverse-
  engineering Sony's own proprietary control channel -- a very large,
  separate undertaking, likely not practical. **Not confirmed/ruled
  out with actual research yet** -- flagged as a strong suspicion to
  verify (or disprove) before concluding it's impossible, not stated as
  fact.

**Next real step when this gets picked up**: trace the real
`ESP32-A2DP` source for AVRCP passthrough command handling (the same
kind of investigation the volume-sync and discovery work above already
did successfully) to confirm exactly what's receivable and wire
transport controls to the existing playback functions first (the likely
quick, real win) -- then separately research whether ANY standard
Bluetooth mechanism exists for ANC/transparency/EQ control from a
source device, treating that as a probably-separate, probably-much-
harder problem rather than assuming it rides along with transport
controls for free.

## "Ooga booga" round -- fast pass through most of the deferred list, user's explicit call to move fast near a session limit

User asked to push the WiFi retry interval to match the normal 6h
resync cadence and then work through the deferred list quickly rather
than carefully one item at a time. All of the below is real code, not
stubs -- same verification discipline as the rest of this file (real
library APIs confirmed from source where a third-party library was
involved), just written faster and documented more tersely. None of it
is hardware-confirmed.

- **WiFi retry unified**: `kSkippedRetryDelayMs` is now literally
  `kResyncIntervalMs` (6h) instead of a separate, shorter constant --
  no reason a skipped attempt should retry faster than one that
  actually ran and failed normally.
- **Headphone transport controls (play/pause/skip) -- real AVRCP
  passthrough, confirmed from the real `ESP32-A2DP` source.**
  `BluetoothA2DPSource::set_avrc_passthru_command_callback(void(*)(uint8_t
  key, bool isReleased))` is real, public API -- registered in
  `BluetoothSource::begin()` alongside the data/volume callbacks.
  `ESP_AVRC_PT_CMD_PLAY`/`PAUSE`/`FORWARD`/`BACKWARD` are the library's
  own real key-code constants (confirmed used directly in its own
  `BluetoothA2DPSink.cpp`). Same stash-for-main-loop pattern as every
  other BT-task callback in this file (can't touch `state`/`MenuEngine`
  from that task) -- `BluetoothSource::drainTransportCommand()`, polled
  by a new `main.cpp::handleBtTransportCommands()` every `loop()`
  iteration, dispatches to the exact same functions the on-screen UI
  already uses (`AudioBridge::pauseResume()`, `MenuEngine::
  playNextInQueue()`/`skipPrevious()`). Play/Pause are real distinct
  AVRCP commands, not a toggle -- each only acts if playback isn't
  already in the requested state, matching real remote-button semantics.
- **Failed-files log on SD** (`/clickpod_failed.txt`, `Library::
  logFailedFile()`) -- option 2 from the FLAC-limitations writeup,
  finally done. Called from both real skip sites (the proactive 24-bit
  skip in `MenuEngine::setNowPlaying()`, the generic grace-period skip
  in `UI.cpp`'s `tickPlaybackClock()`) so finding which files need
  re-encoding no longer means catching the message live in the serial
  monitor.
- **Create new playlist from the device** -- scoped pragmatically, not
  as originally imagined. An EMPTY playlist would be invisible anyway
  (`Library::indexPlaylists()` skips empty `extraPlaylistTracks`
  entries), so "+ New Playlist" lives inside the existing "Add to
  Playlist" submenu (`openTrackMenu()`) and creates a new playlist
  SEEDED with whatever track you were adding -- non-empty from
  creation, shows up immediately. Auto-named ("New Playlist N" via
  `Library::nextNewPlaylistName()`) -- real custom naming needs a
  letter-picker screen this device has no component for (no keyboard),
  which is genuine UX design surface belonging in the simulator first,
  not improvised here under time pressure.
- **Manual "Set Time" UI (plan item 6) -- analog clock face + digital
  readout, built directly in firmware, not simulator-first.** New
  `AppMode::SET_TIME` (`Settings` gained a "Set Time" row ->
  `MenuEngine::enterSetTime()`). Encoder-driven since there's no
  keyboard: RIGHT tap switches the active hand (`state.
  setTimeEditingMinute`), rotating sweeps it (`MenuEngine::
  adjustSetTime()`, wraps within its own range -- hour 0-23, minute
  0-59, doesn't spill into the other hand), CENTER confirms (`
  confirmSetTime()` -> `TimeSync::setManualTime()`), LEFT cancels
  without saving. `Screens::drawSetTime()` draws a real analog face
  (plain radial lines from Arduino trig, same simple-primitives style as
  this file's existing hand-drawn glyphs -- no curve library) with the
  active hand in the accent color and thicker, plus a live `HH:MM`
  digital readout underneath (the plan's own reasoning: "analog looks
  nice but isn't a quick read").

  `TimeSync::setManualTime(hour, minute)`/`currentTimeString()`: a
  SEPARATE fallback path from the real NTP-synced one (not reusing
  `syncedEpochUtc`) -- counts forward from `millis()` the same way a
  real sync does, anchored to the entered time instead of an NTP-
  fetched one, and `currentTimeString()` always prefers a real sync
  over this when one exists. Deliberately HOUR:MINUTE only, no date --
  nothing in this app's UI (statusbar, AOD screen) ever displays a date,
  so there was no real consumer to justify a date-entry UI. **Not
  persisted across reboot** -- there's no RTC either way, so a manual
  set resetting on power-cycle is an honest, already-accepted limitation
  of this whole "no RTC" design, not a new gap this feature introduces.

  **Deliberately skipped the simulator-first step** this project
  normally follows for new UX -- explicit user instruction to move fast
  near a session limit rather than round-trip through the browser
  simulator first. Flagging the gap honestly rather than silently
  skipping it: if this UX needs iteration, port it to the simulator and
  amend `docs/SPEC.md` before changing it further, per the usual
  discipline.
- **AOD screen dimming**: NOT touched this round, and there was nothing
  new to do -- the "AOD" section above already covers this; its
  software half (lock input, keep playing, show a live clock) is
  complete, and screen DIMMING is blocked on backlight hardware
  (GPIO rewiring) that needs the user's physical access, not firmware.
- **ULT WEAR EQ/ANC/transparency**: deliberately NOT attempted --
  the deferred-list writeup above's strong suspicion (headphone-side-
  only via Sony's proprietary protocol, no standard A2DP/AVRCP hook for
  this) was never actually verified against source, and guessing at a
  reverse-engineered vendor protocol blind is a different risk class
  from everything else in this round (which all traced real, confirmed
  library APIs first). Left as the one deferred item still untouched --
  worth real research before attempting, not a "move fast" candidate.

**Not yet hardware-confirmed, same caveat as every change this
session** -- no PlatformIO in this sandbox. Next real step covers all
of the above: flash, and test each independently (headphone play/pause/
skip buttons; check `/clickpod_failed.txt` appears after a known-bad
file skips; create a playlist via a track's "..." menu and confirm it
shows up under Playlists; open Settings -> Set Time, confirm the clock
face and readout track the encoder/buttons correctly and the statusbar
reflects the saved time after confirming).

## First real `pio run` of this whole round, real build error caught and fixed

User's first actual build after the "ooga booga" round failed with
`error: reference to 'map' is ambiguous`. Real, well-known GCC quirk,
not a logic bug: `Library.h`'s `#include <map>` (for its playlist
overlay, `std::map<String, std::vector<Track>>`) puts `std::map`'s
class template in scope in every file that includes it; GCC can't
disambiguate a plain `map(...)` call between that and Arduino's global
`long map(long,long,long,long,long)` function when both are visible
unqualified, even though the class template isn't actually callable.
Fixed by qualifying the two affected call sites to `::map(...)` (forces
global-scope resolution) -- `main.cpp`'s `syncBluetoothToUi()`,
`Screens.cpp`'s `applyBrightness()`. Checked every other `map()` call
site in the codebase (`AudioBridge.cpp`'s two) -- neither transitively
includes `<map>`, so they're unaffected and were left alone.

**Also added**: `platformio.ini` gained `monitor_filters =
esp32_exception_decoder` -- a real, documented PlatformIO/Espressif32
feature that automatically resolves a crash backtrace's raw addresses
against the just-built `.elf` right in the serial monitor. Added after
two real crashes this project could only reason about from unresolved
hex addresses (no PlatformIO in the cloud sandbox that wrote most of
this firmware) -- closes that gap for every future crash on whatever
machine actually runs `pio device monitor` from here on.

## Thirty-fifth real hardware bug (found, fixed): analog clock hands/ticks weren't quite centered -- truncation, not a geometry bug

User reported the Set Time screen's "12 and 00 aren't on the exact
center, there's a bit of an offset." Real, found cause in
`Screens.cpp`'s `drawSetTime()`: every tick-mark/hand endpoint used a
plain `(int)(...)` cast on a trig result -- C-style truncation toward
zero, not rounding to the nearest pixel. This shortens/shifts every
line by a fractional-pixel amount that varies per angle, producing
exactly the described "slightly off, not dramatically" asymmetry
across the 12 ticks and both hands.

**Fixed**: replaced every `(int)(...)` cast on a trig result with
`(int)lroundf(...)` (rounds to nearest, not truncates) -- `lroundf()`
is standard C99 `<math.h>`, already included. While in there, factored
the whole analog-face drawing (ticks + both hands + center dot) out
into a shared `drawAnalogClockFace()` helper, since the AOD screen
needed the identical drawing below -- see the next entry.

## Thirty-sixth real hardware bug/decision: AOD/locked screen gained the analog clock face it was missing, not just the digital readout

User explicitly asked: the powered-off/AOD screen showed a digital
clock but no analog face to "accompany" it, unlike the Set Time screen.
`Screens.cpp`'s `drawOffClock()` now draws a real analog face (via the
same `drawAnalogClockFace()` helper the previous entry introduced,
read-only here -- no highlighted hand) above the digital readout,
parsing `TimeSync::currentTimeString()`'s `"HH:MM"` back into integer
hour/minute (same parsing `MenuEngine::enterSetTime()` already does).
Still fires on the same once-a-minute lightweight-redraw path
(`state.statusbarDirty`) as before -- redraws the whole face region on
each tick rather than tracking/erasing just the previous hand
positions, since this only fires once a minute and the simpler
approach is plenty cheap at that rate. `drawOff()`'s "hold CENTER to
power on"/playing-paused lines shifted down to make room.

**Not yet hardware-confirmed, same caveat as everything in this
section** -- no PlatformIO in the cloud sandbox that wrote this. Next
real step: flash, confirm the clock face now looks properly centered on
BOTH screens, and that the AOD screen's new analog face appears and
advances correctly once a minute while locked.

## Thirty-seventh real hardware bug/decision: queue drag-to-reorder extended to history rows, not just the upcoming queue

User explicitly asked: "I can only pick up upcoming songs... I want it
to work" for history rows too. `MenuEngine.cpp`'s `queueSelectionIsQueueItem()`
(renamed `canGrabSelectedRow()`) previously only allowed grabbing a row
inside the queue segment; now allows grabbing ANY row except the "now"
row itself (which has no position to move to/from). `moveGrabbedQueueItem()`
now branches on which segment the selected combined-list index falls
in and reorders WITHIN that segment (wrapping at its own edges, same
as the original queue-only behavior) -- deliberately does NOT support
dragging a row ACROSS the "now" boundary (history into queue or vice
versa): the two are different-length containers, so that would be a
real move/splice, not a simple swap, and sliding something past the
currently-playing track has no obvious meaning anyway. A grab started
in history stays confined to history; a grab started in queue stays
confined to queue, exactly mirroring the pre-existing queue-only
behavior just now also available on the other side of "now".

**Not yet hardware-confirmed**. Next real step: flash, grab a history
row (RIGHT-tap) and confirm UP/DOWN reorders it within history, and
confirm a grab still refuses to start on the "now" row itself.

## Thirty-eighth real hardware bug (reported, NOT the one the stack-size mitigation targeted -- new evidence, next step is a real backtrace): scrubbing over Bluetooth crashed again, this time explicitly on Core 0

User hit another crash scrubbing/playing over Bluetooth --
`Guru Meditation Error: Core 0 panic'ed (LoadProhibited)`,
`EXCVADDR: 0xbc285320` (a garbage-looking address, not a small offset
from null) -- different from the thirty-second bug's `spinlock_acquire`
assert, though the lead-up looks similar (a `stream ready` resync with
no further decode-info lines printed before the crash). **Important
new data point**: this crash is explicitly on **Core 0**. The thirty-
second bug's mitigation (`getArduinoLoopTaskStackSize()`, bumping the
Arduino `loop()` task's stack) only affects whichever core `loop()`
itself runs on -- by default Core 1 on a dual-core ESP32 Arduino setup,
NOT Core 0. If this crash is genuinely happening inside a Core-0-pinned
task (the BT/Bluedroid stack's own tasks commonly are), that mitigation
could not have addressed it, and -- a real structural limit worth
being honest about -- **the Bluedroid task's own stack size is baked
into Arduino-ESP32's precompiled libraries for this PlatformIO
framework**, not something a build flag or sketch-level override can
change the way `getArduinoLoopTaskStackSize()` changes the Arduino
core's OWN loop task. If the real cause turns out to be a stack
overflow on a Bluedroid-internal task specifically, there is no
application-level fix available for that from here.

Separately observed in the same session: reconnecting to a speaker
took noticeably longer than usual, and stopping Bluetooth afterward
"froze for a good second" before completing -- consistent with real,
if not fully diagnosed, resource pressure around the BT stack under
heavier use, matching this project's long heap-exhaustion history, but
not confirmed as the crash's cause specifically.

**Not fixed blind this round** -- guessing at a second mitigation with
no symbolized trace would be exactly the kind of guess this project
avoids; the one concrete action taken is adding `esp32_exception_decoder`
(see the top of this section) so the literal next crash, whatever it
is, gives a real function-and-line backtrace instead of raw hex. The
busy-message fix for the "froze for a second" UX complaint (see
`MenuEngine.cpp`'s "Turn Bluetooth Off" action, now shows "Stopping
Bluetooth..." before the blocking `end()` call, same established
pattern as the library-rescan busy message) is a real, separate, low-
risk UX fix made this round regardless of the crash investigation --
it doesn't address the crash, just stops a real teardown delay from
looking like a hang.

**Next real step**: get a FRESH crash with the exception decoder now
active -- that resolves this ambiguity immediately (which function, on
which task, is actually faulting) instead of needing another round of
hypothesis-from-raw-hex.

## Manual "Sync Time Now" button added

User's explicit ask, on top of the existing 6h-plus-boot WiFi sync
cadence (confirmed working as designed -- the "resyncing pretty
frequently" report in the same session was almost certainly just an
artifact of the device crashing and rebooting several times in that
test session, each reboot legitimately triggering its own boot-time
sync attempt by design, not a sign the interval itself regressed):
Settings gained a "Sync Time Now" row that forces an immediate attempt
instead of waiting for the next scheduled cycle or a reboot -- e.g.
right after fixing `/clickpod_wifi.txt`, no reason to wait up to 6h.

`TimeSync.cpp`'s background task used to sleep via one long
`vTaskDelay(kResyncIntervalMs)` between cycles; `requestManualSync()`
sets an atomic flag, and the wait is now sliced into 1-second polls
(`waitUpToWithEarlyWake()`) that check it and return early the instant
it's set -- negligible cost (a flag check + a short sleep) for
something fired at most a few times a session. The manual request
still goes through the exact same `radioHeapOk()`/`RadioLock` checks
every other attempt does -- this requests an attempt, it doesn't
bypass the safety guards around one, so it can still be legitimately
skipped (and retried per the normal rules) if the radio isn't available
right now.

**Not yet hardware-confirmed**. Next real step: flash, press "Sync Time
Now" with a known network in range, confirm it attempts immediately
(serial log should show the scan starting right away) rather than
waiting.

## Thirty-ninth real hardware bug (found, fixed): new playlist / added-to playlist didn't show up until navigating away and back

User reported: create a playlist from a track's "..." menu, go back to
Playlists, the new one isn't there yet -- leave to the main menu and
back in, and now it is. Real cause: `state.menuStack` holds frozen
snapshots of each menu level, built once at the moment it's entered
-- navigating back to an already-open level (a plain pop/stack-restore,
which is what `closeTrackMenu()` does to return to wherever you were)
never re-invokes `buildPlaylistList()`, so it kept showing whatever the
Playlists screen looked like BEFORE the new playlist was created (or,
for the quieter version of the same bug, before a track was added to
an EXISTING one, which left its "N tracks" sub-label stale too).

**Fixed**: factored `buildPlaylistList()`'s row-building logic out into
`buildPlaylistItems()`, and added `refreshPlaylistListIfPresent()` --
scans `state.menuStack` (right after `closeTrackMenu()` has restored
it) for any frame titled "Playlists" and rebuilds its items in place
(preserving the selection index where it still fits), instead of
leaving the stale snapshot sitting there until the user happens to
navigate away and back. Called after all three playlist-mutating
actions in `openTrackMenu()`'s "Add to Playlist" submenu: "+ New
Playlist", and both the index-backed and mock-fallback "add to an
existing playlist" rows.

**Not yet hardware-confirmed**. Next real step: flash, create a new
playlist, go straight back to Playlists (no detour through the main
menu) and confirm it's already there; separately add a track to an
existing playlist and confirm its track count updates immediately too.

## Deferred: playlist rename/delete, and vaguer "menus were a bit confusing" feedback on playlist creation

User also flagged, not started this round:

- **No way to rename or delete a playlist once created** -- only
  creation (and adding tracks to one) exists. User's own suggestion:
  reuse the existing drag-and-drop grab mechanism's general pattern for
  the UI gesture, though rename specifically still needs real text
  entry (the same no-keyboard constraint `nextNewPlaylistName()`'s auto-
  naming was built around -- see the "Ooga booga" round's writeup above)
  and delete needs a real confirmation step (destructive, no undo) and
  a decision about what happens to the real on-SD index data for a
  playlist that ALSO exists there (vs. the session-only
  `extraPlaylistTracks` overlay for brand-new ones) -- not scoped yet.
- **"Playlist creation menus were a bit confusing"** -- flagged without
  enough specifics to act on; the one CONCRETE bug inside that
  complaint (new/updated playlists not showing up without navigating
  away and back) is fixed above, but if the menu FLOW itself (which
  screen you land on, what the rows are labeled, etc.) still feels
  wrong after that, needs more specific description (or the usual
  simulator-first UX pass) to actually improve rather than guess at.

## Next session plan (as of 2026-10-01, agreed in a planning-only conversation, nothing below built yet)

A lot got discussed/decided in conversation without any code written this
round -- consolidated here as one list so a fresh session doesn't have to
re-derive it from scratch or lose pieces. Rough priority order, per the
user's own framing ("fix current bugs first, test-tone/real-BT-audio
last"):

1. **DONE, confirmed on real hardware.** Verified the FLAC maxFrameSize
   patch actually works -- the build-time patch script applied, and
   previously-failing files (the known maxFrameSize-too-large ones) now
   play instead of being skipped. User confirmed: "faulty songs play
   now."
2. **DONE.** Converted the build-time patch into a real fork -- user
   forked `schreibfaul1/ESP32-audioI2S` to
   `github.com/ismail3005/esp32-audioi2s`, the same patch is now a real
   commit there (`clickpod-3.0.12-flac-patch` branch), `platformio.ini`'s
   `lib_deps` points at it pinned by commit SHA, and
   `scripts/patch_audioI2S.py` is deleted. Full writeup in the "real
   options for the two FLAC decode limitations" section above (option 3).
3. ~~Real 24-bit FLAC decode support~~ -- **DECIDED AGAINST, not doing
   this.** Was briefly on the plan as the (explicitly flagged highest-
   risk) option #4, but the user reconsidered and is re-encoding their
   24-bit files to 16-bit instead (option #1 from the original FLAC-
   limitations list) -- batch-converting locally with `ffmpeg -map 0
   -c:v copy -sample_fmt s16 -c:a flac` (the `-map 0 -c:v copy` keeps
   embedded cover art intact, which a bare `-sample_fmt s16` conversion
   can otherwise drop). Lower-risk, same end result for their actual
   library. No firmware work needed for this item at all.
4. **DONE, not yet hardware-confirmed.** Bluetooth device picker +
   last-device auto-reconnect -- see the dedicated section above (search
   "Bluetooth device picker + last-device auto-reconnect"). Real
   discovery via `BluetoothSource::startDiscovery()`, a second menu level
   on the existing Bluetooth screen, persisted `state.btDeviceName` used
   by boot-time auto-resume, `ESP32-A2DP`'s `lib_deps` now pinned by
   commit SHA.
5. **DONE -- SD-card-based WiFi credentials for TimeSync.** Plain text
   file, `/clickpod_wifi.txt` (SD root), kept OFF the repo per the user's
   explicit call (a single physical SD card in their pocket vs. permanent
   GitHub history -- their reasoning, and the right call for a repo that
   could go public) -- not in `.gitignore` either since it's never
   written by this codebase in the first place, only read; the user
   creates/edits it by hand on their computer. Format: one network per
   line, `SSID,PASSWORD` (split on the FIRST comma only, so a password
   containing a comma still works), blank lines and `#`-prefixed lines
   ignored. `TimeSync::loadCredentialsFromSd()` parses it.

   **Concurrency design, the actual tricky part**: TimeSync's sync loop
   runs on its own background FreeRTOS task, but this codebase's
   established rule (SD and the TFT share one physical SPI bus, only
   proven safe for strictly sequential single-task access, not two
   concurrent tasks -- see the library-scan-backgrounding gotcha) means
   that background task must never touch the SD card itself. Solved by
   reading the credentials file exactly ONCE, synchronously, on the main
   thread in `main.cpp`'s `setup()` (same sequential pattern as the
   library index build, right next to it, inside the same `if (sdOk)`
   block) -- the parsed result is a plain in-RAM `std::vector<TimeSync::
   WifiCredential>` handed into `TimeSync::begin(credentials)`, and the
   background task only ever reads that already-parsed vector from then
   on, never SD directly.

   **Matching logic** (`TimeSync.cpp`'s `tryOnce()`): on each scan, a
   network matching one of the loaded credentials is preferred over an
   open network (the user's own hotspot is a deliberate, likely more
   reliably-present choice than whatever random open network happens to
   be nearby) -- falls back to the original open-network-only behavior
   if none of the known networks are in range, or if the file doesn't
   exist/SD has nothing on it at all (empty vector, same as before this
   feature existed, zero behavior change for a card with no credentials
   file).

   **Not yet done**: the user hasn't actually created `/clickpod_wifi.txt`
   on their card yet or tested a real hotspot sync -- that's the next
   real-hardware step once this flashes, to confirm the whole path
   (parse -> prefer -> join with password -> NTP) actually works, not
   just compiles.
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
   **Backlight hardware, UPDATED this round**: BL pin is currently
   hardwired to 3.3V (always full brightness, see the hardware-gotchas
   entry above). Module is a Waveshare ILI9341 (confirmed SKU/batch
   `260523WS18366`, Waveshare's 2.4inch LCD Display Module) -- matches a
   known Waveshare board design with an onboard switching transistor
   dedicated to the backlight.

   **Tried the controller-brightness route first, confirmed it does
   NOT work on this board.** `Screens::applyBrightness()` sends the
   ILI9341's own `WRDISBV`/`WRCTRLD` (`0x51`/`0x53`) brightness commands
   over the already-wired SPI bus -- no new wiring, applied at boot and
   live from the Settings brightness slider, specifically so this could
   be tested for free before committing to any GPIO work. User confirmed
   on real hardware: no effect. This conclusively proves (not just
   infers) that this module's backlight bypasses the controller's
   internal PWM driver and goes straight to the external transistor off
   the BL pin, as suspected -- real GPIO wiring is required, there's no
   software-only path. `applyBrightness()` is left in place (harmless,
   literally a no-op on this hardware) rather than ripped out, in case a
   future board swap ever uses a module that *does* route brightness
   through the controller.

   Also resolved what the user remembered as "it used to dim when I held
   CENTER" -- traced to `Screens.cpp`'s `drawOff()` (triggered by exactly
   a CENTER long-press via `togglePower()`): `fillScreen(TFT_BLACK)` plus
   dark-gray (`0x4208`) "hold CENTER to power on" text. An all-black
   image transmits dramatically less light through an LCD than the
   colorful UI even with the backlight itself never changing power --
   this fully explains both specific memories (the "dimming" and the
   "dim text"), confirmed by the user. Not a bug, not hidden brightness
   control -- just how LCD contrast reads to the eye.

   **GPIO decision history -- BOTH candidate strapping pins tried and
   FAILED on real hardware, now PARKED.** Originally considered moving a
   user-facing button (e.g. LEFT) onto a strapping pin to free up its
   regular GPIO for the backlight instead -- user correctly rejected this
   as bad practice: a button is something the user actively presses
   during normal handling (reaching into a bag, fumbling for the power
   switch -- precisely the scenario AOD mode exists to protect against),
   so putting ANY user-facing input on a boot-strapping pin creates a
   real, repeatable "hold this button at the wrong instant -> boot fails"
   failure mode a static hardware fact doesn't have.

   **Attempt 1: GPIO0, no external pull-up.** User wanted "as safe as
   possible, no pullup" -- legitimate, not reckless: the ESP32 boot ROM
   enables its own weak INTERNAL pull-up on GPIO0 during the strapping-
   sample window (why stock dev boards' BOOT buttons work with zero
   external pull-up). Real test: FAILED -- board dropped into download
   mode. Diagnosed as the backlight transistor's base circuit being
   actively biased LOW, strong enough to beat the weak internal pull-up.

   **Attempt 2: a real external 10k pull-up added to GPIO0.** If the
   circuit merely floated, a dedicated 10k pull-up to 3.3V should easily
   dominate it. Real test: FAILED AGAIN -- still download mode. This
   means the circuit's LOW bias is strong enough to beat even a real
   external pull-up, not just the weak internal one -- a meaningfully
   stronger signal than first assumed. (Side incident during this attempt:
   a separate scare -- the board got stuck in a repeating ROM-bootloader-
   banner reset loop after a RST press -- turned out to be a loose
   breadboard connection from the physical rework, not a strapping issue;
   recovered by a full power-cycle and reseating jumpers. Worth
   remembering: breadboard contact flakiness can produce symptoms that
   look exactly like a strapping problem.)

   **Attempt 3: pivoted to GPIO12 instead, no pull-up.** Reasoning at the
   time: if the circuit idles LOW (just demonstrated against GPIO0), that
   should be exactly what GPIO12 (MTDI) wants (LOW at reset), for free.
   Real test: FAILED differently -- the `pio run -t upload` step itself
   started hanging, consistent with `VDD_SDIO` reading the wrong strap
   value and leaving the flash chip's SPI I/O at the wrong voltage for
   reliable communication (GPIO12 uniquely affects flash I/O during
   upload too, not just app boot -- a nastier failure mode than GPIO0's).

   **Conclusion: stop guessing at strapping pins for this signal.** Two
   real failures on two different pins/polarities means this backlight
   circuit's actual behavior during the ESP32's brief reset-sampling
   window isn't reliably predictable from reasoning about pull
   directions, and a steady-state DC read wouldn't necessarily catch a
   timing/capacitance-related difference either (plus the above reminder
   that some of this round's symptoms may have been breadboard flakiness,
   not logic). **Current physical/firmware state: reverted and back in
   sync** -- BL is back on the 3.3V rail (always full brightness, no
   software control), `PIN_TFT_BL` is removed from `Pins.h`,
   `Screens::applyBrightness()` is back to just the (confirmed harmless
   no-op) SPI commands, and `AnoInput.cpp`'s LEFT pull-up is back to
   normal (its resistor was briefly borrowed for the GPIO0 pull-up test,
   now returned).

   **Next real attempt, when picked back up, should NOT be a third
   strapping pin.** Free up an ordinary, already-used, non-strapping GPIO
   instead -- candidate: `PIN_TFT_RST` (GPIO4), which only gets toggled
   once, deliberately, by firmware during `TFT_eSPI::begin()`, well after
   the ESP32's own boot-strap sampling is over, and is a simple one-shot
   digital pulse, not something that needs to survive an uncertain
   passive bias fight at reset the way the backlight apparently does.
   Move TFT_RST onto GPIO12 instead (lower-stakes there -- affects reset
   timing, not flash I/O voltage during normal operation) and give the
   now-vacated GPIO4 to the backlight -- a completely ordinary GPIO with
   zero boot-strap risk either way. NOT independently verified that the
   display module's own RST pin doesn't have its own pull resistor that
   could cause the exact same class of problem on a different signal --
   do the same disconnect-and-multimeter-check discipline before wiring
   this blind, don't repeat this round's pattern of guessing and testing
   live on hardware three times in a row.

   **One more operational lesson from this round, unrelated to pin
   choice**: right after reverting all the GPIO0/12 code and physical
   wiring back to the known-good state, `pio run -t upload` started
   hanging with ZERO esptool handshake output at all (not even
   "Connecting......"), a different symptom from every earlier failure
   this round. Root cause: a breadboard connection near all the GPIO0
   rework (most likely the auto-reset circuit's own EN/GPIO0 lines,
   which share that same crowded area) got physically disturbed --
   confirmed by flashing with the board fully off the breadboard
   (presumably a cleaner direct connection to the USB-to-serial adapter),
   which worked immediately; board was then reseated back onto the
   breadboard afterward with no further issue. **If a future upload ever
   hangs with no handshake output at all** (distinct from a normal
   "Connecting......" retry loop), suspect a disturbed physical
   connection from recent rework before suspecting firmware or strapping
   pins -- pull the board off the breadboard and flash it directly as a
   fast diagnostic.
8. **Real Bluetooth audio** -- DONE, see the dedicated section above
   (search "Real Bluetooth audio -- DONE"). Not deprioritized anymore --
   picked back up, found the library's own documented hook, implemented,
   and iterated on real hardware feedback (glitchy audio, a real crash)
   across several rounds.
9. **Create new playlists from the device itself.** User flagged this
   while the Bluetooth crash/audio-quality work above was still being
   worked through -- explicitly grouped it with items 6/7 (manual
   "Set time" UI + AOD) as later work, after Bluetooth is solid.
   Currently playlists can only be ADDED TO from a track's "..." menu
   ("Add to Playlist") -- there's no way to create a brand-new, empty
   (or first-track) playlist from the device; `Library.cpp`'s playlist
   model (both the mock `PLAYLISTS` data and the real on-SD-index path,
   see the "on-SD compact index" section above) and the "Add to
   Playlist" submenu in `MenuEngine.cpp` would need a real look before
   attempting this -- not scoped yet, not started.

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
