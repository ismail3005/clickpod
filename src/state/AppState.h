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
    // TODO(step 7): battery is a placeholder until the MAX17048 fuel gauge
    // is wired in -- see README status item 7.
    int battery = 82;
    // TODO: btOn/btConnectedTo are placeholders driven by the UI's own mock
    // BT_DEVICES list (Library::BT_DEVICES), not the real BluetoothSource
    // A2DP link -- that's still a separate, isolated bring-up path
    // (kTestWiredPlayback in main.cpp). Wiring them together is follow-up
    // work once BT output is merged into normal playback instead of being
    // its own test mode.
    bool btOn = false;
    String btConnectedTo; // empty = not connected

    int brightness = 70;
    String sortPref = "Artist";
    bool darkMode = false;

    MenuReturn btReturn;
    MenuReturn trackMenuReturn;

    bool dirty = true; // set whenever state changes in a way that should trigger a redraw
};

extern AppState state;
