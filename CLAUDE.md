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
src/ui/Library.*         real SD-scanned Artist/Album/Track/Playlist data
                         (Library::scanFromSd()), mock data kept as a fallback
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
pattern). The Bluetooth screen (`MenuEngine::enterBluetooth()`) is ONE
row for `BluetoothSource::kTargetDeviceName` (currently `"ULT WEAR"`)
with its real status, not a multi-device picker -- `ESP32-A2DP`'s source
mode connects to one hardcoded target by name, it doesn't enumerate
discoverable devices (spec section 8 amended to document this). Tapping
that row calls `BluetoothSource::begin()`; "Turn Bluetooth Off" calls
`BluetoothSource::end()`. The old `kTestWiredPlayback` compile-time
branch in `main.cpp` is gone -- wired output (`AudioBridge`) is always
available now, and BT is purely a runtime UI toggle on top of it.

**Still not real**: BT only streams a 440Hz test tone
(`BluetoothSource.cpp`'s `provideTestTone()`), not actual decoded audio.
Making it stream real music means routing `ESP32-audioI2S`'s PCM output
into the A2DP source's data callback instead of out to the I2S DAC --
a real dual-output audio pipeline change, not done, not trivial (the two
libraries currently have no shared hook point for this). Scope it
properly before attempting -- don't half-wire it.

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
