#pragma once

// UI mode state machine (SPEC.md section 5): all ANO input behavior is
// dispatched based on the current mode, not ad-hoc flags.
enum class AppMode {
    BOOT,
    MENU,
    NOW_PLAYING,
    LYRICS,
    QUEUE,
    BT_PAIRING,
    SETTINGS,
    OFF,
};
