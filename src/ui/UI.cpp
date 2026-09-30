#include "UI.h"

#include "../audio/AudioBridge.h"
#include "../state/AppState.h"
#include "AlbumArt.h"
#include "InputRouter.h"
#include "MenuEngine.h"
#include "Screens.h"

namespace UI {
namespace {

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
        // "Lyrics don't track" -- the active-line highlight is entirely
        // position-driven (drawLyrics() picks it from state.now.posSec
        // each time it draws), but the Lyrics screen was never actually
        // being redrawn on an ordinary position tick: only progressDirty
        // got set below, and Screens::render() only acts on progressDirty
        // for AppMode::NOW_PLAYING, not LYRICS -- so posSec was updating
        // correctly the whole time, the screen just never repainted to
        // show it. Uses the heavier `dirty` flag (full redraw) rather
        // than a lighter partial one like Now Playing's progress bar --
        // every visible lyric line's Y position shifts together whenever
        // the active line changes (it's a centered scrolling view), so a
        // true partial redraw isn't as simple here. Real tradeoff, lower
        // impact than Now Playing's flicker was (this screen isn't where
        // most playback time is spent) -- candidate for the same
        // partial-redraw treatment later if it's noticeable in practice.
        state.dirty = true;
    } else {
        // Just the position moved -- progressDirty triggers the cheap
        // partial redraw instead of a full-screen flicker every tick.
        state.progressDirty = true;
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
    }
    Screens::render();
}

} // namespace UI
