#pragma once

#include <Arduino.h>
#include <vector>

#include "../ui/UiTypes.h"

// UI mode state machine (SPEC.md section 5): all ANO input behavior is
// dispatched based on the current mode, not ad-hoc flags. Ported from the
// browser simulator's `state.mode` -- BT and TRACK_MENU are overlay modes
// reachable from anywhere (see MenuReturn below), not steps in a fixed
// menu hierarchy.
enum class AppMode {
    BOOT,
    MENU,
    NOW_PLAYING,
    LYRICS,
    QUEUE,
    BT,
    TRACK_MENU,
    OFF,
};

struct NowPlaying {
    bool hasTrack = false;
    String key; // Library::keyFor(track); used to look up lyrics
    String artist;
    String album;
    String title;
    char art = '\x01';
    uint16_t durSec = 0;
    float posSec = 0;
    bool playing = false;
    String path; // real SD path if known (see Track::path); empty for placeholder tracks
};

// Snapshot of where to unwind back to after leaving a globally-reachable
// overlay (Bluetooth, the track context menu) -- popping the menu stack
// alone doesn't work since these can be entered from any mode and replace
// the stack outright. Same pattern as the simulator's state.btReturn /
// state.trackMenuReturn.
struct MenuReturn {
    bool valid = false;
    AppMode mode = AppMode::MENU;
    std::vector<Menu> stack;
};

struct AppState {
    AppMode mode = AppMode::BOOT;
    AppMode lastActiveMode = AppMode::MENU; // mode to restore to on power-on

    std::vector<Menu> menuStack;

    NowPlaying now;
    std::vector<Track> queue;
    int queueSelected = 0;
    bool queueGrabbed = false; // true while the selected queue row is picked up for reordering
    std::vector<Track> history; // previously played tracks this session, for "skip previous"

    int volume = 62; // 0-100 UI scale; mapped to the DAC's 0-21 range in AudioBridge
    // Synced from the real MAX17048 reading each loop() iteration (see
    // main.cpp's syncBatteryToUi()) once Battery::begin() succeeds; stays
    // at this default otherwise (gauge not wired/not responding).
    int battery = 82;
    // Synced from the real BluetoothSource/A2DP link each loop() iteration
    // (main.cpp's syncBluetoothToUi()) -- not a mock device list anymore.
    // BT still only streams a test tone, not real decoded audio -- see
    // BluetoothSource.h.
    bool btOn = false;
    String btConnectedTo; // empty = not connected

    int brightness = 70;
    String sortPref = "Artist";
    bool darkMode = false;
    // UTC offset for the statusbar clock (TimeSync::currentTimeString()),
    // -12..+14 -- doesn't cover half-hour zones (e.g. India UTC+5:30),
    // a deliberate simplification. Not persisted across reboots (no NVS/
    // flash write for settings exists yet); resets to 0 each boot.
    int utcOffsetHours = 0;

    MenuReturn btReturn;
    MenuReturn trackMenuReturn;

    bool dirty = true; // set whenever state changes in a way that needs a full-screen redraw
    // Lighter-weight than `dirty`: set by the once-a-second Now Playing
    // position tick, which only needs the progress bar/time strip
    // redrawn, not the whole screen -- see Screens::render() and
    // UI.cpp's tickPlaybackClock. Avoids a full-screen flicker every
    // second during playback.
    bool progressDirty = false;
    // Lighter still: set when the selection cursor moves within the same
    // menu list (UP/DOWN/rotate), which only needs the old + new selected
    // rows redrawn, not the whole list -- see Screens::render()'s
    // updateMenuSelection(). Falls back to a full `dirty` redraw itself
    // if the viewport needs to scroll to keep the new selection visible.
    bool selectionDirty = false;
};

extern AppState state;
