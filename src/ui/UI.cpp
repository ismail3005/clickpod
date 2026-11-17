#include "UI.h"

#include <math.h>

#include "../audio/AudioBridge.h"
#include "../bt/BluetoothSource.h"
#include "../net/TimeSync.h"
#include "../state/AppState.h"
#include "AlbumArt.h"
#include "InputRouter.h"
#include "Library.h"
#include "MenuEngine.h"
#include "Screens.h"

namespace UI {
namespace {

int lastLyricsActiveIdx = -1; // see tickPlayback()'s LYRICS branch

constexpr uint32_t kBootMs = 1100;   // matches the simulator's boot->MENU timeout
constexpr uint32_t kClockMs = 500;   // matches the simulator's playback-clock setInterval
// Safety net only: a real file whose stream claims to be running but has
// produced no audio at all after this long is given up on. Ordinary
// failures (file won't open, decoder rejects it) don't wait for this --
// they show up as a stopped stream and are handled on the spot.
constexpr uint32_t kNoAudioGiveUpMs = 6000;

uint32_t bootAt = 0;
uint32_t lastClockMs = 0;
bool booted = false;

// Watches the real stream behind the current track, EVERY loop pass (not
// on the 500ms display tick): end of track, and files that fail. Returns
// true if it moved playback on (the caller skips the rest of this tick).
//
// End of track is "the decoder stopped by itself", not "the displayed
// position reached the displayed duration" as it used to be -- that
// comparison ran twice a second against a duration truncated to whole
// seconds, so it routinely cut the last fraction of a second off a track
// (and could not advance at all on a file whose duration was unknown).
// Following the stream itself also makes the gap between tracks as short
// as the decoder allows.
bool watchStream() {
    NowPlaying &n = state.now;
    if (n.path.length() == 0) return false; // placeholder track: simulated clock only, nothing to watch

    using AudioBridge::Status;
    Status st = AudioBridge::status();
    if (st == Status::Starting) return false; // queued, will start within a moment

    if (!n.playbackConfirmed && st == Status::Playing && AudioBridge::currentTimeSec() > 0) {
        n.playbackConfirmed = true;
        MenuEngine::notePlaybackProgress();
    }

    if (st == Status::Playing) {
        if (!n.playbackConfirmed && millis() - n.startedAtMs >= kNoAudioGiveUpMs) {
            MenuEngine::handleTrackFailure("no audio after several seconds (decode failure)", true);
            return true;
        }
        return false;
    }
    if (st == Status::Failed) {
        MenuEngine::handleTrackFailure("could not be opened", true);
        return true;
    }
    if (st == Status::Idle) {
        // setNowPlaying() refused to start it (unsupported format --
        // already logged there, and not a sign of a bad card).
        MenuEngine::handleTrackFailure(nullptr, false);
        return true;
    }
    if (st == Status::Ended) {
        // (A track only a second or two long can be over before the
        // decoder's whole-second position counter ever leaves zero.)
        if (n.playbackConfirmed || (n.durSec > 0 && n.durSec <= 2)) {
            MenuEngine::playNextInQueue(); // normal end of track
        } else {
            // Stopped without ever producing audio: the decoder rejected
            // the file (e.g. a format detail this library can't handle).
            MenuEngine::handleTrackFailure("never started playing (decode failure)", true);
        }
        return true;
    }
    return false; // Paused while the UI says playing: nothing to do here
}

void tickPlayback() {
    // Deliberately no early-out for AppMode::OFF anywhere in here -- the
    // AOD/locked screen keeps playback running exactly as-is, which
    // includes moving on to the next track. Nothing below draws; it only
    // updates state and sets redraw flags that OFF mode ignores.
    AudioBridge::tick();
    MenuEngine::serviceDeferredWork();

    if (!state.now.playing || !state.now.hasTrack) return;
    if (watchStream()) return;

    uint32_t now = millis();
    if (now - lastClockMs < kClockMs) return;
    lastClockMs = now;

    NowPlaying &n = state.now;
    const bool realFile = n.path.length() > 0;

    if (realFile) {
        // A non-FLAC file has no STREAMINFO to read the length from up
        // front; pick it up from the decoder once it has worked it out.
        if (n.durSec == 0) {
            uint32_t d = AudioBridge::durationSec();
            if (d > 0 && d < 65536) n.durSec = (uint16_t)d;
        }
        // The decoder's real position -- except while the user is
        // scrubbing (the bar follows the wheel; the seek isn't committed
        // yet) and while a committed seek is still settling (the decoder
        // reports its OLD position until it has refilled and decoded the
        // first frame at the new one -- syncing from it then makes the
        // bar jump back and forward again).
        if (!state.scrubPending) {
            float real = (float)AudioBridge::currentTimeSec();
            if (state.seekSettleUntilMs) {
                bool arrived = fabsf(real - (float)state.seekTargetSec) <= 2.0f;
                bool timedOut = (int32_t)(now - state.seekSettleUntilMs) >= 0;
                if (arrived || timedOut) state.seekSettleUntilMs = 0;
                else real = n.posSec;
            }
            n.posSec = real;
        }
    } else {
        // Placeholder/mock track: nothing real behind it, simulate.
        n.posSec += kClockMs / 1000.0f;
        if (n.durSec > 0 && n.posSec >= n.durSec) {
            MenuEngine::playNextInQueue();
            return;
        }
    }

    if (state.mode == AppMode::LYRICS) {
        // Redraw only when the highlighted line actually changes (every
        // few seconds), not on every tick -- a full-body redraw twice a
        // second is a visible flicker. Same computation drawLyrics() uses.
        auto it = Library::LYRICS.find(n.key);
        int activeIdx = (it != Library::LYRICS.end()) ? MenuEngine::activeLyricIndex(it->second) : -1;
        if (activeIdx != lastLyricsActiveIdx) {
            lastLyricsActiveIdx = activeIdx;
            state.dirty = true;
        }
    } else {
        state.progressDirty = true; // just the position strip
    }
}

// BluetoothSource::discoveredName() callback runs on the BT stack's own
// task, so the device-picker screen can't just redraw reactively when a
// new device shows up -- this polls the already-stashed count each
// update() tick (cheap int compare) and only rebuilds the menu/redraws
// when it actually changed, same "poll a plain counter, don't touch the
// producing context" pattern AnoInput's encoder delta already uses.
int lastDiscoveredCount = -1;

// Nothing else ever sets `dirty` just because time passed -- drawStatusbar()
// only ever ran as a side effect of some OTHER full redraw (a mode change,
// track change, etc). That meant the statusbar clock could sit frozen at
// whatever it showed on the last full redraw indefinitely, even well after
// TimeSync::isSynced() flipped true in the background -- "it says synced in
// the monitor but the UI never shows a time" was this, not a sync bug. This
// polls the actual displayed text (cheap: one String compare) every tick and
// only flags a redraw when it actually changed (sync completing, or a new
// minute), same polling pattern already used for the BT device picker above.
String lastStatusbarClock;

void tickStatusbarClock() {
    String nowText = TimeSync::currentTimeString();
    if (nowText != lastStatusbarClock) {
        lastStatusbarClock = nowText;
        state.statusbarDirty = true;
    }
}

void tickBluetoothDevicePicker() {
    bool onPicker = state.mode == AppMode::BT && !state.menuStack.empty() &&
                    state.menuStack.back().title == "Choose Device";
    if (!onPicker) {
        lastDiscoveredCount = -1; // reset so re-entering the picker always does a fresh rebuild
        // The picker was left -- by LEFT, a double-tap to Now Playing,
        // locking the screen, anything -- without choosing a device: shut
        // the scan and the Bluetooth stack it needed back down (see
        // BluetoothSource::stopDiscovery()). One pass late on purpose:
        // the teardown blocks for a moment, so let the screen that was
        // navigated to get drawn first.
        static bool stopNextPass = false;
        if (!BluetoothSource::isDiscovering()) {
            stopNextPass = false;
        } else if (!stopNextPass) {
            stopNextPass = true;
        } else {
            stopNextPass = false;
            BluetoothSource::stopDiscovery();
        }
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
        AnoInput::discardEvents(); // nothing pressed during the splash should fire at the first screen
        if (millis() - bootAt >= kBootMs) {
            booted = true;
            state.mode = AppMode::MENU;
            state.dirty = true;
        }
    } else {
        InputRouter::update();
        tickPlayback();
        tickStatusbarClock();
        tickBluetoothDevicePicker();
    }
    Screens::render();
}

} // namespace UI
