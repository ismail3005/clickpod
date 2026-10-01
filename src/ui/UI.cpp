#include "UI.h"

#include "../audio/AudioBridge.h"
#include "../bt/BluetoothSource.h"
#include "../state/AppState.h"
#include "AlbumArt.h"
#include "InputRouter.h"
#include "Library.h"
#include "MenuEngine.h"
#include "Screens.h"

namespace UI {
namespace {

int lastLyricsActiveIdx = -1; // see tickPlaybackClock()'s LYRICS branch

constexpr uint32_t kBootMs = 1100;   // matches the simulator's boot->MENU timeout
constexpr uint32_t kClockMs = 500;   // matches the simulator's playback-clock setInterval
// kPlaybackStartGraceMs itself now lives in UI.h (public) -- MenuEngine.cpp
// needs it too, see the comment there. Not independently measured on
// hardware; sized generously (normal playback should start in well under
// a second) specifically to avoid a false-positive skip on a legitimately
// slow-starting file. If a real file that DOES eventually play gets
// skipped, raise this; if a failed file takes noticeably longer than this
// to get skipped, it can come down.

uint32_t bootAt = 0;
uint32_t lastClockMs = 0;
bool booted = false;

void tickPlaybackClock() {
    uint32_t now = millis();
    if (now - lastClockMs < kClockMs) return;
    lastClockMs = now;

    if (state.mode == AppMode::OFF) return;
    if (!state.now.playing || !state.now.hasTrack) return;

    // Detect a real file that failed to actually start decoding (see
    // AppState.h's NowPlaying comment) and skip it rather than stall
    // silently forever. Only applies to real files (path set) -- placeholder/
    // mock tracks have nothing to confirm against AudioBridge for.
    if (state.now.path.length() > 0 && !state.now.playbackConfirmed) {
        if (AudioBridge::isRunning()) {
            state.now.playbackConfirmed = true;
        } else if (millis() - state.now.startedAtMs >= kPlaybackStartGraceMs) {
            Serial.printf("[audio] \"%s\" never started playing -- skipping (see CLAUDE.md: some "
                          "real files fail to decode, e.g. a FLAC frame too large for this library)\n",
                          state.now.title.c_str());
            MenuEngine::playNextInQueue(); // sets state.dirty itself
            return;
        }
    }

    // Real position for a real file (confirmed API, see AudioBridge.h) --
    // simulated increment only as a fallback for placeholder/mock tracks
    // with no real path. Without this the displayed position was always
    // the UI's own guess, never what's actually playing -- which is also
    // why scrubbing looked like it worked but didn't actually move the
    // audio: the real decoder position and the UI's posSec were two
    // unrelated numbers.
    if (state.now.path.length() > 0) {
        state.now.posSec = (float)AudioBridge::currentTimeSec();
    } else {
        state.now.posSec += kClockMs / 1000.0f;
    }
    // durSec==0 means unknown, not "already over" -- true for placeholder/
    // mock tracks, or a real track whose FlacMeta::readStreamInfo() call
    // in MenuEngine::setNowPlaying() failed. Without this guard playback
    // would auto-skip to the next track within the first tick of starting.
    if (state.now.durSec > 0 && state.now.posSec >= state.now.durSec) {
        MenuEngine::playNextInQueue(); // sets state.dirty itself -- new track needs a full redraw
    } else if (state.mode == AppMode::LYRICS) {
        // "Lyrics don't track" (fixed earlier) needed the Lyrics screen to
        // actually redraw on a position tick -- Screens::render() only
        // acted on the lighter progressDirty for AppMode::NOW_PLAYING, not
        // LYRICS, so posSec was updating correctly but nothing repainted
        // to show it. The first fix set `dirty` (full redraw) on EVERY
        // tick while on this screen, which fixed the tracking but visibly
        // flickered -- a full-body fillRect + text redraw every ~500ms is
        // exactly the class of bug the Now Playing progress bar already
        // had fixed once before (see CLAUDE.md's second hardware bug).
        // Fixed properly here: only actually redraw when the active line
        // (MenuEngine::activeLyricIndex(), the same computation
        // drawLyrics() itself uses -- one source of truth, not duplicated
        // logic that could drift) has actually changed, which for real
        // lyrics happens every several seconds, not twice a second. Still
        // a full-body redraw when it DOES fire (every visible line's Y
        // shifts together, since this is a centered scrolling view, not
        // independent rows -- a true partial/row-level redraw isn't as
        // simple here as Now Playing's single progress strip was), just
        // not on every tick regardless of whether anything changed.
        auto it = Library::LYRICS.find(state.now.key);
        int activeIdx = (it != Library::LYRICS.end()) ? MenuEngine::activeLyricIndex(it->second) : -1;
        if (activeIdx != lastLyricsActiveIdx) {
            lastLyricsActiveIdx = activeIdx;
            state.dirty = true;
        }
    } else {
        // Just the position moved -- progressDirty triggers the cheap
        // partial redraw instead of a full-screen flicker every tick.
        state.progressDirty = true;
    }
}

// BluetoothSource::discoveredName() callback runs on the BT stack's own
// task, so the device-picker screen can't just redraw reactively when a
// new device shows up -- this polls the already-stashed count each
// update() tick (cheap int compare) and only rebuilds the menu/redraws
// when it actually changed, same "poll a plain counter, don't touch the
// producing context" pattern AnoInput's encoder delta already uses.
int lastDiscoveredCount = -1;

void tickBluetoothDevicePicker() {
    bool onPicker = state.mode == AppMode::BT && !state.menuStack.empty() &&
                    state.menuStack.back().title == "Choose Device";
    if (!onPicker) {
        lastDiscoveredCount = -1; // reset so re-entering the picker always does a fresh rebuild
        return;
    }
    int count = BluetoothSource::discoveredCount();
    if (count != lastDiscoveredCount) {
        lastDiscoveredCount = count;
        MenuEngine::refreshBluetoothDevicesMenu();
    }
}

} // namespace

void begin(TFT_eSPI &tft) {
    Screens::begin(tft);
    AlbumArt::begin(tft);

    MenuEngine::buildMainMenu();
    state.mode = AppMode::BOOT;
    state.dirty = true;
    bootAt = millis();
    lastClockMs = millis();
    Screens::render();
}

void update() {
    if (!booted) {
        if (millis() - bootAt >= kBootMs) {
            booted = true;
            state.mode = AppMode::MENU;
            state.dirty = true;
        }
    } else {
        InputRouter::update();
        tickPlaybackClock();
        tickBluetoothDevicePicker();
    }
    Screens::render();
}

} // namespace UI
