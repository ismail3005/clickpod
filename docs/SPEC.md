# DIY iPod-Classic-Style MP3 Player — Full Specification

## 1. Project Overview

A fully custom, DIY-built portable MP3 player inspired by the iPod Classic's
form factor and interaction model, built around an ESP32-WROVER. Not a
touchscreen device — navigation is entirely via a rotary encoder + directional
buttons (Adafruit ANO breakout), matching the classic "scroll wheel" feel.
Currently in breadboard prototyping phase, moving to perfboard, with a
possible future custom PCB revision. Enclosure will be a fully custom,
toolless, user-serviceable 3D printed design (separate work-stream from
firmware, described for context in section 8).

This document is the complete functional/technical spec for the FIRMWARE.
Treat every section as a hard requirement unless marked "nice to have" or
"future/stretch."

---

## 2. Hardware Bill of Materials (confirmed)

| Component | Part | Interface | Notes |
|---|---|---|---|
| MCU | ESP32-WROVER-B (N4) | — | 4MB Flash, 4MB PSRAM (confirmed via `ESP.getPsramSize()` → 4194304). Classic Bluetooth (BR/EDR) + BLE supported. NRND part but functionally fine. |
| Audio DAC | PCM5102A | I2S | 32-bit/384kHz capable. Look for modules with separate decoupled digital/analog supply rails. |
| Display | ILI9341, 2.4" TFT, non-touch | SPI | 240×320 resolution. No onboard SD slot on this display module — SD is a fully separate module (see below), sharing the SPI bus but on its own breakout. |
| SD storage | Standalone microSD reader module (SPI) | SPI | Separate physical module, not integrated with the display. Shares SPI bus (MOSI/MISO/SCK) with the display via its own independent CS pin — same bus-sharing principle as before, just two physically separate boards instead of one combined board. |
| microSD card | SanDisk Ultra 128GB + adapter | — | Genuine card, no fake-capacity risk. Speed class irrelevant for this use case (audio streaming needs far less throughput than card provides). |
| Input | Adafruit ANO Rotary Navigation Encoder | Rotary encoder (quadrature) + 5 momentary buttons (up/down/left/right/center) | This is the ONLY physical input device on the device (see section 5 for full mapping). No other buttons planned besides possibly a separate hardware power/reset consideration — TBD, currently planned to be handled entirely through the ANO center button (long-press). |
| Battery | LiPo, 906090 size code (90×60×9mm), ~6000mAh, JST-PH connector | — | Chosen over a 505080/3000mAh option because real energy-density math confirmed the 505080 listing's 3000mAh claim was inflated (~1350-2200mAh realistic for that volume), whereas 6000mAh is plausible and consistent for the 906090 volume. **Amendment:** originally assumed bare wire leads; battery actually terminates in a JST-PH connector -- see section 3.1 for how this changes the physical wiring (no cutting/soldering directly to the battery leads required).|
| Charge IC | TP4056 + boost module (steps battery's 3.0-4.2V up to stable 5V) | — | Deliberately NOT using power-path management (MCP73871/PowerBoost-style simultaneous charge+use) — user decided this adds unnecessary cost/complexity for a personal device; user will simply avoid using the device while it's charging, similar to how iPod-era devices were typically used. |
| Fuel gauge | MAX17048 | I2C | Sits in parallel across the battery's raw +/- terminals (NOT inline with the charge/boost power path) — purely a sensing tap, reports battery % to ESP32 over I2C. Independent of whichever charge circuit is used. |
| Bluetooth audio | None — uses ESP32's built-in Classic Bluetooth via A2DP source profile (`ESP32-A2DP` library) | — | No additional BT hardware. AVRCP (via the same library) handles remote control events (though in this project, physical device controls take priority — see BT section). |

### Key libraries anticipated
- `ESP32-audioI2S` (schreibfaul1) — audio decode + I2S playback. Library
  supports FLAC playback; metadata extraction depth (Vorbis comments,
  PICTURE block, STREAMINFO) needs verification — see section 6a
- `ESP32-A2DP` (pschatzmann) — Bluetooth A2DP source + AVRCP
- `SD` / `SdFat` — file system access
- `TFT_eSPI` — ILI9341 display driver (2.4" variant)
- `TJpg_Decoder` — album art JPEG decode (source: FLAC PICTURE metadata
  block, NOT ID3 APIC — see section 6a)
- A FLAC metadata parsing library/approach (may need custom/manual parsing
  of Vorbis comment + PICTURE + STREAMINFO blocks if `ESP32-audioI2S`
  doesn't expose these directly — see section 6a)
- Adafruit MAX17048 library (or equivalent) — fuel gauge I2C driver
- Bounce2 or custom debouncing — for ANO button inputs (if not handled by ANO's own logic)

---

## 3. Power Architecture

```
Battery (bare wires, 3.0-4.2V) ──┬── TP4056+boost module ── stable 5V ── ESP32 5V pin
                                  └── MAX17048 (I2C sense only, parallel tap)
```

- Battery connects to BOTH the TP4056+boost module (B+/B-) AND the MAX17048
  (battery+/GND) in parallel — NOT in series. Both are independent taps on
  the same two battery terminals.
- TP4056's OUT+/OUT- (post-boost, stable ~5V) feeds the ESP32's 5V pin —
  NOT through the ESP32's own USB-C port. The ESP32's own USB-C port is used
  ONLY for flashing/serial during development.
- TP4056 module's own USB-C/micro-USB input is the device's SOLE charging
  port in the final design.
- No power-path management: charging and running simultaneously is not a
  supported/designed-for use case. This is a deliberate, accepted trade-off.
- MAX17048 reports state-of-charge % via I2C; this should be surfaced in the
  UI (see section 6).

### 3.1 Physical Wiring (confirmed on actual hardware -- amendment)

The original text above described the power architecture conceptually
("battery connects to both in parallel"); this section captures how that's
actually implemented with the real parts in hand, confirmed during
breadboard bring-up.

**Battery connector:** the battery does NOT have bare wire leads as
originally assumed in section 2 -- it terminates in a JST-PH connector.

**MAX17048 board (Adafruit clone) has two battery JST-PH ports, wired in
parallel internally** -- not two different circuits, just a convenience
pass-through so the battery connection can be handed on to the next stage
without splicing wires. This is what makes the "parallel tap" in the
diagram above physically simple:

```
Battery (JST-PH) ── MAX17048 battery port 1
                     MAX17048 battery port 2 ── (wired to) ── TP4056+boost B+/B-
```

Since the TP4056+boost module in use here only has solder pads (B+/B-,
OUT+/OUT-), not its own JST port, the link from MAX17048 port 2 to the
TP4056's B+/B- pads is two wires soldered directly onto MAX17048 port 2's
PCB pads (no JST housing needed) and onto the TP4056's B+/B- pads. Polarity
was verified with a multimeter continuity check against the already-known
polarity of battery port 1 before connecting the battery -- getting this
backwards is a real fire-risk mistake, not just a "doesn't work" one.

**Clarifying B+/B- vs OUT+/OUT- on the TP4056+boost module**, since this was
a point of confusion during bring-up: `B+`/`B-` is the bidirectional battery
terminal -- current flows INTO the battery through these pins while USB is
connected (charging), and OUT of the battery through the same pins while
running on battery power (discharging, feeding the internal boost
converter). It is not a charge-only connection. `OUT+`/`OUT-` is the
boosted ~5V output that actually feeds the ESP32 and rest of the circuit --
this is the pin pair described as "TP4056's OUT+/OUT-" above.

**MAX17048 header pins** (VIN, GND, SCL, SDA, INT, QSTART -- separate from
the two battery JST ports):
- `VIN` -> ESP32 3.3V (powers the sensor IC's own logic; distinct from the
  battery-sense connection on the JST ports)
- `GND` -> common ground
- `SCL` -> GPIO 27, `SDA` -> GPIO 21 (see `src/config/Pins.h`)
- `INT` -> left unconnected for now (optional low-battery alert interrupt,
  not needed for basic percentage polling)
- `QSTART` -> left unconnected (forces a manual fuel-gauge quick-start
  calibration; the chip already does this automatically on power-up)

**Open item carried forward, not yet resolved:** some TP4056+boost combo
modules have a physical push-button power switch on the boost output
(double-tap to cut power entirely), which would add a second, non-firmware
"off" state on top of the deep-sleep-based power on/off described in
section 5.4. Need to confirm whether this specific module has that button
before finalizing the power on/off design.

---

## 4. Firmware Bring-Up Order (recommended sequence — already partially in progress)

1. ESP32 + PSRAM verification (DONE — confirmed 4MB PSRAM detected)
2. ESP32 + SD card: read file list over serial, confirm SD access works
   BEFORE touching audio
3. ESP32 + PCM5102 via `ESP32-audioI2S`: play a file directly from SD.
   This is flagged as the highest-risk/most failure-prone step — isolate
   it fully before adding anything else.
4. Add ILI9341 display: confirm SD + display coexist properly on shared
   SPI bus (separate CS pins, no contention) — render song info once
   playback already works.
5. Add ESP32-A2DP Bluetooth output as a SEPARATE playback path, only once
   wired (I2S/PCM5102) playback is fully solid. Don't debug both audio
   paths simultaneously.
6. Add ANO encoder + buttons last — pure input logic, lowest risk, easiest
   to bolt onto an already-working playback core.
7. Add MAX17048 battery monitoring (I2C) — independent subsystem, low risk,
   can be added at any point once I2C bus is free (check for conflicts with
   any other I2C devices).

---

## 5. Input Mapping — Adafruit ANO (rotary encoder + up/down/left/right/center)

This is the most detailed and important section. The ANO is the ONLY input
device. All behavior is context-dependent based on current UI mode
(`MENU`, `NOW_PLAYING`, `BT_PAIRING`, `SETTINGS`, `OFF`, etc. — implement as
a proper state machine, not ad-hoc flags).

### 5.1 In MENU mode (browsing lists: file browser, playlists, artist/album
sort views, settings, BT device list, etc.)

| Input | Action |
|---|---|
| Rotate (CW/CCW) | Fast scroll through list items |
| Up / Down (single press) | Move one item at a time (slower, precise scroll) |
| Up / Down (long press) | Fast scroll (same effective behavior as rotating) |
| Left / Right | Navigate menu levels — Left = back/up a level, Right = enter/forward (dpad-style navigation) |
| Center (single tap) | Select / confirm current item |
| Center (long press) | Power on/off (see section 5.4) |
| Right (long press) | Enter Bluetooth pairing/discoverable mode (see 5.4 — RESOLVED: dedicated to Right specifically, chosen because it has no other long-press assignment anywhere in the app, unlike Up/Down which are used for Queue/Lyrics in NOW_PLAYING mode). Available globally, not just from a BT settings screen. |
| Left (long press) | **ADDED.** On a track row specifically (file browser, playlist contents) — open the track context menu (see 5.5). No-op on non-track rows (Artist/Album/Settings/etc). |

### 5.2 In NOW_PLAYING mode (a track is loaded/active, whether playing or paused)

| Input | Action |
|---|---|
| Rotate (CW/CCW) | Scrub through current track position |
| Up / Down | Volume up/down |
| Left / Right (single tap) | Skip to next/previous track. **AMENDED** (was originally fast-forward/rewind on tap) -- rotate already covers scrubbing position, so tap L/R is free for track skip instead, matching classic iPod physical-button behavior (tap to skip, as opposed to touch-wheel seek). Right long press stays reserved globally for BT pairing (5.4), never reassigned here. |
| Center (single tap) | Play/Pause toggle |
| Center (double tap, within ~300-400ms window) | Return to menu — music CONTINUES PLAYING in background, does not pause |
| Center (long press) | Power on/off (see section 5.4) |
| Down (long press) | Show lyrics screen (see section 6.5) |
| Up (long press) | Show queue screen (see section 6.6) |
| Left (long press) | **ADDED.** Open the track context menu (see 5.5) for the currently playing track. |

### 5.3 Center button state machine (most complex single input)

Center button must distinguish THREE distinct behaviors:
1. **Single tap** → Play/Pause (in NOW_PLAYING) or Select (in MENU)
2. **Double tap** (second press-release within ~300-400ms of first
   release) → Return to menu without pausing (NOW_PLAYING mode only)
3. **Long press** (hold beyond a threshold, e.g. 600-800ms — tune by feel) →
   Power on/off

Implementation approach: on press-release, start a short timer. If a second
press-release lands within the window, fire double-tap action immediately
and cancel the pending single-tap timer. If no second tap arrives, fire
single-tap action once timer expires. Track raw hold-duration independently
for long-press detection (this fires on hold, not on release, so it doesn't
conflict with the tap-counting logic — but ensure a long-press doesn't ALSO
fire a spurious single/double tap on release).

### 5.4 Power on/off and BT pairing

- **Power ON** (from fully-off/deep-sleep state): long-press Center. NOTE:
  since the device may be in deep sleep with no active polling loop, the
  Center button's GPIO must be configured as a valid ESP32 deep-sleep wake
  source (`esp_sleep_enable_ext0_wakeup` or equivalent) — this is a hardware/
  config requirement, not just software logic. Verify the ANO's center
  button pin is wake-capable on the chosen GPIO.
- **Power OFF** (from any active state): long-press Center. On power-off,
  save current playback state (track, position, volume, current menu
  context) to flash/NVS (e.g. ESP32 Preferences library) before entering
  deep sleep. RESOLVED: resume last playback state on next power-on if this
  is straightforward to implement reliably; if it proves complex/fragile
  (e.g. edge cases around corrupted state, SD card swapped while off), fall
  back to a clean restart to main menu instead. Implementer's judgment call
  on which is "easy enough" — default to attempting resume-state first.
- **BT pairing mode**: RESOLVED — long-press on the **Right** button
  specifically (not Center, not a global "any dpad button" gesture, and not
  Up/Down since those are taken by Queue/Lyrics in NOW_PLAYING mode). This
  is a single, unambiguous, globally-available gesture with no conflicts
  anywhere in the app.

### 5.5 Track context menu ("..." menu) — ADDED

Not in the original spec; worked out and validated in the UI simulator once
"add to playlist / play next / add to queue, like most phone music players"
came up as a real want.

**Trigger:** long-press Left. Scoped to wherever a specific track is
contextually meaningful:
- A track row in a file/library browser or playlist contents list (not on
  non-track rows like Artist/Album/Settings entries)
- The currently playing track, from NOW_PLAYING
- The highlighted entry in the Queue screen (see 6.7 below)

**Menu contents:**
1. **Play Next** — insert at the front of the queue
2. **Add to Queue** — append to the end of the queue
3. **Add to Playlist** — pushes a submenu listing existing playlists; selecting
   one appends the track to it
4. **Cancel** — dismiss, no action

**Navigation within the menu:** identical to normal MENU mode (rotate/Up/Down
to move, Center to select, Left to go back a level or dismiss entirely if
already at the top of the context menu). Dismissing (by any path) returns
to exactly where the menu was opened from — the underlying screen/selection
state isn't disturbed.

---

## 6. Screens / UI Modes Required

1. **Boot/splash screen** — shown briefly on power-on
2. **Main menu** — entry point after boot; likely a list: Music, Playlists,
   Bluetooth, Settings (adjust as needed)
3. **File/library browser** — navigable list of tracks, with sort options:
   - By Artist
   - By Album
   - By other available metadata (genre, year, etc. — whatever tags exist
     after MusicBrainz Picard tagging)
4. **Playlists menu** — user-defined or imported playlists
5. **Now Playing screen** — displays:
   - Track title, artist, album (from FLAC metadata — see section 6a below)
   - Album art (from embedded FLAC PICTURE metadata block — see 6a)
   - Playback progress bar + elapsed/remaining time (see 6a — FLAC makes
     this simple, no bitrate-guessing needed)
   - Battery percentage (from MAX17048)
   - Current volume level indicator
6. **Lyrics screen** — accessed via long-press Down from Now Playing.
   **AMENDED (return gesture):** exits back to Now Playing via CENTER tap,
   LEFT tap (the app's normal back gesture), or holding Down again --
   symmetric with how the screen was opened. Not specified in the original
   text; caught as a real dead-end (no way out once in) while validating
   the flow in the UI simulator.
   RESOLVED, and confirmed via direct file inspection (`metaflac --list`
   on a real track): lyrics live in the Vorbis comment field **`LYRICS`**,
   and — better than initially assumed — they are **full LRC-format synced
   lyrics with timestamps**, e.g.:
   ```
   LYRICS=[ti:Get Lucky (feat. Pharrell Williams and Nile Rodgers)]
   [ar:Daft Punk; Pharrell Williams; Nile Rodgers]
   [by:SpotiFlac]

   [00:32.18]Like the legend of the phoenix, huh
   [00:36.25]Our ends were beginnings
   ...
   ```
   This means the Lyrics screen SHOULD do real-time line-highlighting/
   auto-scroll synced to playback position (parse each `[mm:ss.xx]` tag,
   compare against current playback time, highlight/scroll to the
   matching line) — a genuinely nice feature, not just a static text dump.
   Parsing approach: split the `LYRICS` field on newlines, regex each line
   for a leading `[mm:ss.xx]` timestamp, store as a sorted list of
   (timestamp_ms, text) pairs, then during playback find the last entry
   whose timestamp ≤ current position. A few header lines (`[ti:...]`,
   `[ar:...]`, `[by:...]`) precede the timed lines and should be skipped/
   ignored when parsing (they're LRC metadata tags, not lyric lines —
   recognizable because they don't match the `[mm:ss.xx]` numeric pattern).

### 6a. Audio Format Note: FLAC (read before implementing metadata/duration)

The user's entire library is **FLAC**, not MP3. This changes several things
from a typical ESP32 MP3 player tutorial (most online examples assume
MP3/ID3v2), so flagging explicitly:

- **File organization**: `Artist/Album/track.flac` folder structure, already
  properly tagged (artist/album/title/etc. metadata is present in the files
  themselves via FLAC's native tagging system, called **Vorbis comments** —
  NOT ID3 tags; ID3 is an MP3-specific convention, FLAC uses a different
  metadata block system entirely. Do not assume `ESP32-audioI2S`'s ID3
  parsing callbacks apply here — FLAC metadata needs to be read via that
  library's FLAC-specific handling, or parsed manually from the file's
  metadata blocks if the library's FLAC metadata support is thin. Check
  `ESP32-audioI2S`'s actual FLAC support depth before assuming feature
  parity with its MP3/ID3 handling — this is a real risk area to validate
  early, not assume.
  **Confirmed via real-file inspection**: fields like `ARTIST` and
  `COMPOSER` can appear as MULTIPLE repeated comment entries within the
  same block (one per collaborator — e.g. three separate `ARTIST=` lines
  for a featured-artist track), rather than one comma-separated value.
  Parser should collect all instances of a given field name and join/
  display them (e.g. "Daft Punk, Pharrell Williams, Nile Rodgers") rather
  than assuming exactly one value per field name.
- **Album art**: stored in FLAC's **PICTURE metadata block** (a
  standardized block type within the FLAC container, conceptually similar
  in purpose to MP3's ID3 APIC frame but a completely different binary
  structure/parsing approach). `TJpg_Decoder` still applies once you've
  extracted the raw JPEG bytes from the PICTURE block — the decode step is
  unchanged, only the extraction step differs from the MP3/ID3 plan.
- **Duration / progress bar — this is actually SIMPLER with FLAC than MP3.**
  Background for context: MP3 files come in two flavors —
  **CBR (Constant Bitrate)**, where every second of audio uses the same
  fixed bitrate, making duration a simple calculation
  (duration = file_size_in_bits ÷ bitrate); and **VBR (Variable Bitrate)**,
  where the bitrate fluctuates throughout the file to save space, meaning
  that simple calculation doesn't work and you need to parse a special
  header (Xing/VBRI frame) for an accurate duration. FLAC sidesteps this
  entire problem: as a lossless format, FLAC stores the **exact total
  sample count** directly in its mandatory **STREAMINFO metadata block**
  (along with sample rate), so duration = total_samples ÷ sample_rate —
  exact, no estimation, no CBR/VBR distinction to worry about at all. This
  is a genuine simplification versus the original MP3-based plan.
- **Practical implication for Claude Code**: verify `ESP32-audioI2S`
  exposes FLAC STREAMINFO (duration, sample rate) and metadata block
  parsing (Vorbis comments + PICTURE block) directly. If its FLAC support
  turns out to be playback-only without rich metadata extraction, a
  fallback plan is to parse the FLAC metadata blocks manually before
  handing the file to the audio library for playback (FLAC's metadata
  block format is well-documented and openly specified — this is very
  achievable even if the audio library doesn't expose it natively, just
  more manual work). Flag this to the user as a checkpoint once explored,
  since it affects implementation approach/time estimate.
7. **Queue screen** — accessed via long-press Up from Now Playing. Shows
   upcoming tracks in the current playback queue. **AMENDED (return
   gesture):** CENTER no longer exits this screen -- see selection behavior
   below. LEFT tap, or holding Up again, return to Now Playing.
   **AMENDED (selection):** the queue is a selectable list like everywhere
   else -- rotate/Up/Down move a cursor, and Center jumps playback straight
   to the highlighted track (same as tapping a track in the queue view of
   most phone music players). Tracks skipped over land in playback history
   (so "previous" from Now Playing can still walk back through them);
   everything after the selected track stays queued behind it. Long-press
   Left on a queue entry opens the track context menu (5.5) for it, same as
   everywhere else a track appears.
8. **Bluetooth menu** — list of paired/available devices, connect/disconnect,
   pairing mode trigger
9. **Settings menu** — general device settings (exact contents TBD/flexible,
   at minimum should include something like Bluetooth management, maybe
   display brightness, sort preferences)

### General UI notes
- Screen state machine should track current mode explicitly (enum), since
  ANO input behavior is entirely mode-dependent (see section 5).
- Consider using Lopaka (web-based UI builder, supports U8g2/Adafruit GFX
  code generation) or SquareLine Studio (LVGL-based, more powerful, better
  color display support) to prototype screen layouts before hand-coding —
  user is evaluating both; check with user which was chosen if relevant to
  how UI code should be structured.
- Don't redraw full screen every loop tick for live-updating elements
  (progress bar, elapsed time) — redraw only the specific changed region,
  at a low refresh rate (~1Hz is plenty for a time counter), to avoid
  wasting CPU/SPI bandwidth.

---

## 7. Bluetooth Behavior

- ESP32 acts as an **A2DP source** (streaming audio TO Bluetooth headphones/
  speakers), using `ESP32-A2DP` library.
- AVRCP support exists in the same library for remote-control events.
  RESOLVED: headphone-side controls (play/pause/next/prev via AVRCP) should
  be fully honored alongside the device's own physical controls — both
  control paths act on the same shared playback state. Since simultaneous
  input from both sources in the same instant is realistically very rare,
  implement a simple debounce/tolerance window (e.g. ignore a second
  play/pause toggle from either source within ~300-500ms of the first) to
  prevent an edge-case double-toggle (e.g. device button and headphone
  button both firing a "pause" within the same instant, which would
  otherwise cancel out and resume playback unintentionally). This tolerance
  margin is a sufficient solution per user — no need for more complex
  arbitration logic.
- Bluetooth behavior RESOLVED — phone-style manual model:
  - BT is OFF by default. Long-press Right (per 5.4) turns BT on and enters
    discoverable/pairing mode.
  - Once on, a BT menu screen lists previously-paired devices AND any newly
    discovered ones. Selecting an already-paired device from the list
    connects to it directly (no re-pairing needed). Selecting a new device
    initiates pairing.
  - BT can be toggled off from the same menu, disconnecting any active
    connection.
  - This is a manual, user-driven toggle — no automatic connection
    on boot or automatic switching based on proximity/availability.
- Wired (PCM5102/headphone jack) and Bluetooth are two separate output
  paths, mutually exclusive at any given moment — since BT connection is
  always a deliberate manual user action (per above), switching output to
  BT happens naturally at the moment the user manually connects a device
  from the BT menu; switching back to wired is likewise a manual
  disconnect/toggle-off action. No automatic detection/switching logic
  needed beyond reacting to the user's explicit BT menu actions.

---

## 8. Mechanical/Enclosure Context (for firmware awareness only — NOT
   Claude Code's task, but relevant context)

- Enclosure will be a fully custom, toolless, user-serviceable 3D printed
  design (Fusion 360, printed on a Bambu P1S).
- Front + back panels are planned to seat together (exact joining mechanism
  still being designed — likely simple snap-fit or screw-from-inside along
  main seam, with a small dedicated sliding hatch specifically for SD card
  access, to avoid a visible external SD slot — mirrors the original iPod's
  design philosophy of hiding storage access behind full disassembly, but
  toolless).
- Perfboard (or eventual custom PCB) will mount via internal rail/channel
  system (3 rails on front panel, 1 on back panel, sliding fit) — no
  heat-set inserts or screws for internal component retention.
- Display + encoder mounting approach still being finalized: leaning toward
  mounting these to the front panel independently (via small printed
  retention tabs, not glue), connected to the perfboard via short wire
  jumpers/small connector, so the perfboard remains independently
  removable/serviceable.
- None of this directly affects firmware, EXCEPT: physical button/encoder
  wiring should use connectors (not permanently soldered point-to-point)
  where practical, to match the "serviceable" design goal.

---

## 9. Explicitly Deferred / Not Doing (to avoid scope creep)

- No touchscreen — physical input only (ANO)
- No power-path management (charge-while-use) — accepted trade-off
- No capacitive/genuine-iPod-clickwheel replication — using ANO instead
  (was investigated, real reverse-engineered info exists for real iPod
  wheels, but decided against for build-speed reasons)
- No custom PCB yet — breadboard → perfboard first, PCB is a possible
  future revision once design is validated
- No "YouTube-to-MP3" or similar tooling — music acquisition is being
  handled by the user separately via legitimate sources (Bandcamp, personal
  CD rips, etc.) + MusicBrainz Picard for tagging/album art; this is NOT
  part of the firmware/device scope at all, files simply need to be
  properly tagged (ID3v2 with embedded APIC album art) by the time they're
  on the SD card

---

## 10. Open Questions / Ambiguities for Claude Code to Flag Back to User

Most prior open questions have been resolved (see updated sections 5, 6,
6a, 7 above). Remaining items:

1. **`ESP32-audioI2S` FLAC metadata support depth** — needs hands-on
   verification (does it expose Vorbis comments + PICTURE block + STREAMINFO
   directly, or is manual FLAC metadata block parsing required as a
   fallback?). See section 6a for full detail. This is a genuine technical
   risk/unknown to resolve early in bring-up, not a design ambiguity to ask
   the user about — this one's on the implementer to investigate.
2. Exact Settings menu contents beyond BT management — flexible/TBD, to be
   figured out iteratively as the project develops. Not a blocker.

---

## 11. Non-Negotiable Constraints (summary, for quick reference)

- Hardware is FIXED as listed in section 2 — do not suggest alternate
  components unless something is proven electrically non-functional during
  bring-up.
- Bring-up order (section 4) should be followed — don't attempt to build
  all subsystems simultaneously.
- Input mapping (section 5) is the core UX spec — treat it as close to
  final, but flag genuine conflicts (section 10) rather than silently
  resolving them.
- Battery/power architecture (section 3) is deliberately simple (no
  power-path management) — do not add complexity here unless explicitly
  requested.
