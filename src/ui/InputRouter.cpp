#include "InputRouter.h"

#include "../audio/AudioBridge.h"
#include "../bt/BluetoothSource.h"
#include "../state/AppState.h"
#include "MenuEngine.h"

namespace InputRouter {
namespace {

constexpr uint32_t kFastScrollRepeatMs = 120; // matches the simulator's FAST_SCROLL_REPEAT_MS

AnoButton repeatButton = AnoButton::COUNT; // COUNT = "no button currently fast-scrolling"
uint32_t repeatLastMs = 0;

// Scrubbing (rotate() in NOW_PLAYING mode) used to call AudioBridge::
// seekTo() -- a REAL decoder seek -- on every single encoder detent. A
// periodic throttle (first attempt) didn't actually fix the reported
// "still laggy as fuck" scrubbing, because the real cost isn't call
// FREQUENCY -- it's that each individual seekTo() call plausibly blocks
// the main loop/task for a real stretch (finding the target byte offset
// in the file and resyncing the FLAC decoder isn't instant), and that
// block happens on the SAME task InputRouter::update() runs on. Throttling
// to "at most once per N ms" doesn't help when a single call can itself
// take closer to that same N ms or more -- the UI still stalls mid-spin
// waiting on it.
//
// Fixed properly with a debounced "commit on pause" pattern instead of a
// periodic one: rotate() NEVER calls seekTo() directly anymore -- it only
// updates the displayed position (free) and records what the real
// decoder should eventually seek to. InputRouter::update() (called every
// loop() iteration regardless of input) checks once per call whether
// rotation has been idle for kScrubIdleCommitMs; only once the user
// actually stops turning does the ONE real seek fire, landing on the
// final position. This means a fast continuous spin never blocks on a
// real seek at all -- only the brief pause at the end does -- so the UI
// stays responsive throughout the whole scrub gesture, not just at a
// capped rate.
// 150ms (the original value) still let a long, steady scrub across most of a
// track fire many real seeks -- each one re-enters ESP32-audioI2S's FLAC
// resync path (flac_correctResumeFilePos() scans forward for the next
// 0xFF/0xF8 syncword, a false-positive-prone 16-bit match against compressed
// audio data, then FLACDecoderReset()+InBuff.resetBuffer()), which is a
// lightly-exercised, seek-only code path -- see CLAUDE.md's heap-corruption
// writeup. Raised to cut how many times a single scrub gesture re-enters
// that path, not just to feel smoother.
constexpr uint32_t kScrubIdleCommitMs = 400;
bool scrubSeekPending = false;
uint16_t scrubPendingSec = 0;
uint32_t lastScrubRotateMs = 0;

void startHoldRepeat(AnoButton b, int dir) {
    repeatButton = b;
    repeatLastMs = millis();
    MenuEngine::moveSelection(dir);
}

void togglePower() {
    if (state.mode == AppMode::OFF) {
        state.mode = state.lastActiveMode;
        Serial.println(F("[ui] power on"));
    } else {
        state.lastActiveMode = state.mode;
        state.mode = AppMode::OFF;
        state.queueGrabbed = false;
        Serial.println(F("[ui] power off"));
    }
    state.dirty = true;
}

void rotate(int dir) {
    if (state.mode == AppMode::OFF) return;
    if (state.mode == AppMode::MENU || state.mode == AppMode::BT) {
        MenuEngine::moveSelection(dir > 0 ? 1 : -1);
    } else if (state.mode == AppMode::NOW_PLAYING) {
        // 1 second/tick, not 3 -- "finer" scrubbing per user feedback.
        // Simple fixed step, not variable/accelerating like some iPod-style
        // scroll wheels -- if 1s/tick feels too slow to cross a long track,
        // that's the next thing to try (not attempted here).
        state.now.posSec = constrain(state.now.posSec + dir * 1, 0.0f, (float)state.now.durSec);
        // Does NOT call AudioBridge::seekTo() directly anymore -- see the
        // big comment above kScrubIdleCommitMs for why a periodic throttle
        // here wasn't enough. Just records what the real decoder should
        // eventually seek to; InputRouter::update() commits it once
        // rotation goes idle.
        if (state.now.path.length() > 0) {
            scrubPendingSec = (uint16_t)state.now.posSec;
            scrubSeekPending = true;
            lastScrubRotateMs = millis();
            // See AppState.h's comment -- tells UI.cpp's tickPlaybackClock()
            // to stop syncing posSec from the real decoder position while a
            // scrub is in flight, so its periodic sync can't fight this
            // rotate()'s own posSec update (the "rubberbanding" bug: the
            // real decoder hasn't moved yet, the seek is debounced, so the
            // display kept snapping back to the stale real position).
            state.scrubPending = true;
        }
        // progressDirty (light redraw: just the position/time strip), not
        // the full state.dirty -- scrubbing only changes the displayed
        // position, nothing else on this screen, so there's no reason to
        // pay for a full-body fillRect+text redraw on every single tick.
        state.progressDirty = true;
    } else if (state.mode == AppMode::QUEUE) {
        MenuEngine::moveQueueSelection(dir > 0 ? 1 : -1);
    } else if (state.mode == AppMode::SET_TIME) {
        MenuEngine::adjustSetTime(dir);
    }
}

void handleTap(AnoButton btn) {
    if (state.mode == AppMode::OFF) return; // only CENTER long wakes it

    if (state.mode == AppMode::MENU || state.mode == AppMode::BT || state.mode == AppMode::TRACK_MENU) {
        Menu *m = MenuEngine::currentMenu();
        if (btn == AnoButton::UP) {
            MenuEngine::moveSelection(-1);
        } else if (btn == AnoButton::DOWN) {
            MenuEngine::moveSelection(1);
        } else if (btn == AnoButton::LEFT) {
            // On a slider/choice row, LEFT decrements (mirrors RIGHT/CENTER
            // incrementing) instead of navigating back -- otherwise there
            // was no way to ever bring a slider back down. Only rows that
            // AREN'T currently adjustable fall through to the normal back
            // gesture.
            MenuItem *item = (m && !m->items.empty()) ? &m->items[m->selected] : nullptr;
            if (item && item->isSlider) {
                MenuEngine::adjustSlider(*item, -item->sliderStep);
            } else if (item && item->isChoice) {
                MenuEngine::cycleChoice(*item, -1);
            } else if (state.mode == AppMode::BT) {
                // BT mode can now be TWO levels deep (status screen, then
                // the real device picker -- see MenuEngine::
                // enterBluetoothDevicePicker()), same pop-one-level-first
                // pattern TRACK_MENU already uses below, instead of always
                // jumping all the way out of Bluetooth in one LEFT press.
                // Cancel any in-progress discovery scan when backing out
                // of the picker specifically -- no point letting it keep
                // scanning once the user's no longer looking at the list.
                if (state.menuStack.size() > 1) {
                    BluetoothSource::cancelDiscovery();
                    state.menuStack.pop_back();
                } else {
                    MenuEngine::exitBluetooth();
                }
            } else if (state.mode == AppMode::TRACK_MENU) {
                if (state.menuStack.size() > 1) state.menuStack.pop_back();
                else MenuEngine::closeTrackMenu();
            } else if (state.menuStack.size() > 1) {
                state.menuStack.pop_back();
            }
            state.dirty = true;
        } else if ((btn == AnoButton::RIGHT || btn == AnoButton::CENTER) && m && !m->items.empty()) {
            MenuItem &item = m->items[m->selected];
            if (item.isSlider) MenuEngine::adjustSlider(item, item.sliderStep);
            else if (item.isChoice) MenuEngine::cycleChoice(item, 1);
            else if (item.action) item.action();
        }
    } else if (state.mode == AppMode::NOW_PLAYING) {
        if (btn == AnoButton::UP) {
            state.volume = min(100, state.volume + 5);
            AudioBridge::setVolumePercent(state.volume);
            state.dirty = true;
        } else if (btn == AnoButton::DOWN) {
            state.volume = max(0, state.volume - 5);
            AudioBridge::setVolumePercent(state.volume);
            state.dirty = true;
        } else if (btn == AnoButton::LEFT) {
            MenuEngine::skipPrevious();
        } else if (btn == AnoButton::RIGHT) {
            MenuEngine::playNextInQueue();
        } else if (btn == AnoButton::CENTER) {
            state.now.playing = !state.now.playing;
            AudioBridge::pauseResume();
            state.dirty = true;
        }
    } else if (state.mode == AppMode::LYRICS) {
        // CENTER or LEFT (the app's usual back gesture) both return to Now
        // Playing -- matches how every other screen exits.
        if (btn == AnoButton::CENTER || btn == AnoButton::LEFT) {
            state.mode = AppMode::NOW_PLAYING;
            state.dirty = true;
        }
    } else if (state.mode == AppMode::QUEUE) {
        // Queue behaves like a selectable list: UP/DOWN move the cursor,
        // CENTER plays the highlighted track directly -- LEFT is the way
        // back out instead, since CENTER is now spoken for. RIGHT toggles
        // "grabbing" the highlighted row for reordering (no touchscreen to
        // drag with) -- while grabbed, UP/DOWN move the row itself instead
        // of the cursor, and LEFT/CENTER are ignored so a drag can't be
        // interrupted early.
        // RIGHT only actually grabs when the cursor is on an upcoming-queue
        // row -- history/now rows can't be reordered (queueSelectionIsQueueItem()),
        // so this is a no-op there instead of grabbing something immovable.
        if (btn == AnoButton::RIGHT) {
            if (MenuEngine::queueSelectionIsQueueItem()) { state.queueGrabbed = !state.queueGrabbed; state.dirty = true; }
        }
        else if (state.queueGrabbed) {
            if (btn == AnoButton::UP) MenuEngine::moveGrabbedQueueItem(-1);
            else if (btn == AnoButton::DOWN) MenuEngine::moveGrabbedQueueItem(1);
        }
        else if (btn == AnoButton::UP) MenuEngine::moveQueueSelection(-1);
        else if (btn == AnoButton::DOWN) MenuEngine::moveQueueSelection(1);
        else if (btn == AnoButton::LEFT) { state.mode = AppMode::NOW_PLAYING; state.queueGrabbed = false; state.dirty = true; }
        // CENTER now dispatches across the combined history+now+queue list,
        // not just state.queue -- this is what lets a row from BEFORE the
        // currently-playing track be jumped back to directly.
        else if (btn == AnoButton::CENTER) MenuEngine::playFromCombinedIndex(state.queueSelected);
    } else if (state.mode == AppMode::SET_TIME) {
        // RIGHT switches which hand rotate() sweeps (tap, not rotation --
        // matches the plan's "tap to switch which hand is active"); CENTER
        // confirms (saves via TimeSync::setManualTime() and exits); LEFT
        // cancels without saving. UP/DOWN deliberately unused here --
        // three actions, three buttons, no reason to overload a fourth.
        if (btn == AnoButton::RIGHT) {
            state.setTimeEditingMinute = !state.setTimeEditingMinute;
            state.dirty = true;
        } else if (btn == AnoButton::CENTER) {
            MenuEngine::confirmSetTime();
        } else if (btn == AnoButton::LEFT) {
            MenuEngine::exitSetTimeWithoutSaving();
        }
    }
}

void handleLongPress(AnoButton btn) {
    if (btn == AnoButton::CENTER) { togglePower(); return; }
    // AOD/locked-screen requirement: ignore every input except the CENTER
    // long-press wake gesture while "off". handleTap()/rotate() already
    // had this guard; this one didn't -- a RIGHT long-press while
    // supposedly locked (e.g. tossed in a bag) would silently wake
    // Bluetooth and jump into its menu, defeating the whole point of a
    // locked screen protecting against pocket/bag button presses.
    if (state.mode == AppMode::OFF) return;
    if (btn == AnoButton::RIGHT) { MenuEngine::enterBluetooth(); return; } // global

    if (state.mode == AppMode::MENU || state.mode == AppMode::BT) {
        if (btn == AnoButton::UP) startHoldRepeat(AnoButton::UP, -1);
        else if (btn == AnoButton::DOWN) startHoldRepeat(AnoButton::DOWN, 1);
        else if (btn == AnoButton::LEFT && state.mode == AppMode::MENU) {
            // "..." track menu (Play Next / Add to Queue / Add to
            // Playlist) -- only for rows that are actually a track, not
            // e.g. "Music"/"Settings".
            Menu *m = MenuEngine::currentMenu();
            if (m && !m->items.empty()) {
                MenuItem &item = m->items[m->selected];
                if (item.isTrack) MenuEngine::openTrackMenu(item.trackData);
            }
        }
    } else if (state.mode == AppMode::NOW_PLAYING) {
        if (btn == AnoButton::DOWN) { state.mode = AppMode::LYRICS; state.dirty = true; }
        else if (btn == AnoButton::UP) {
            // Anchor the cursor on the "now playing" row when opening the
            // Queue screen, not row 0 of the upcoming queue -- the combined
            // list now has history above it and queue below it, and landing
            // in the middle is what makes "scroll up for what already
            // played, down for what's next" read naturally on open.
            state.mode = AppMode::QUEUE;
            state.queueSelected = (int)state.history.size();
            state.queueGrabbed = false;
            state.dirty = true;
        }
        else if (btn == AnoButton::LEFT && state.now.hasTrack) {
            Track t{state.now.artist, state.now.album, state.now.title, state.now.durSec, state.now.art, state.now.path};
            MenuEngine::openTrackMenu(t);
        }
        // RIGHT long stays reserved globally for BT pairing; scrubbing is
        // already covered by the encoder.
    } else if (state.mode == AppMode::LYRICS) {
        // Holding the same button that opened the screen closes it again.
        if (btn == AnoButton::DOWN) { state.mode = AppMode::NOW_PLAYING; state.dirty = true; }
    } else if (state.mode == AppMode::QUEUE) {
        if (btn == AnoButton::UP) { state.mode = AppMode::NOW_PLAYING; state.queueGrabbed = false; state.dirty = true; }
        else if (btn == AnoButton::LEFT && !state.queueGrabbed) {
            // Works on any row now -- history and the "now playing" row
            // included, not just the upcoming queue, since all three are
            // part of the same selectable list.
            Track t = MenuEngine::trackAtCombinedIndex(state.queueSelected);
            if (t.title.length()) MenuEngine::openTrackMenu(t);
        }
    }
}

void handleDoubleTap() {
    if (state.mode == AppMode::NOW_PLAYING) {
        state.mode = AppMode::MENU; // playback keeps going
        if (state.menuStack.empty()) MenuEngine::buildMainMenu();
        state.dirty = true;
    } else if (state.now.hasTrack && state.mode != AppMode::OFF && state.mode != AppMode::BOOT) {
        // Same gesture, reversed: jump straight back to whatever's loaded
        // (playing or paused) from anywhere -- MENU (however deep),
        // Lyrics, Queue, BT, a track context menu. Without this there was
        // no way back to Now Playing except starting a new track, which
        // reset the queue -- e.g. adding something to the queue from a
        // different album/playlist and then wanting to return to what was
        // already playing.
        state.mode = AppMode::NOW_PLAYING;
        state.dirty = true;
    }
}

} // namespace

void update() {
    int16_t delta = AnoInput::takeEncoderDelta();
    while (delta > 0) { rotate(1); delta--; }
    while (delta < 0) { rotate(-1); delta++; }

    for (uint8_t i = 0; i < (uint8_t)AnoButton::COUNT; i++) {
        AnoButton b = (AnoButton)i;
        if (AnoInput::wasLongPressed(b)) handleLongPress(b);
        if (AnoInput::wasTapped(b)) handleTap(b);
    }
    if (AnoInput::centerWasDoubleTapped()) handleDoubleTap();

    // Fast-scroll-while-held (MENU/BT UP/DOWN), and its "release" cutoff --
    // mirrors the simulator's startHoldRepeat/stopHoldRepeat pair.
    if (repeatButton != AnoButton::COUNT) {
        if (!AnoInput::isHeld(repeatButton)) {
            repeatButton = AnoButton::COUNT;
        } else if (millis() - repeatLastMs >= kFastScrollRepeatMs) {
            repeatLastMs = millis();
            MenuEngine::moveSelection(repeatButton == AnoButton::UP ? -1 : 1);
        }
    }

    // Commits a pending scrub seek once rotation has paused for
    // kScrubIdleCommitMs -- see the big comment near kScrubIdleCommitMs's
    // declaration. Runs every loop() iteration (cheap check, the real
    // seek only actually fires once per scrub gesture).
    // Known, accepted edge case: if the user skips to a DIFFERENT track
    // within this same kScrubIdleCommitMs window right after releasing
    // the encoder, this fires against the new track instead of being
    // cancelled -- needs a sub-150ms button-press-right-after-scrub-
    // release sequence to trigger, and the consequence is a single
    // incorrect seek the user can immediately re-scrub past, not worth
    // the added bookkeeping (tracking which track a pending seek belongs
    // to) to close for now.
    if (scrubSeekPending && millis() - lastScrubRotateMs >= kScrubIdleCommitMs) {
        AudioBridge::seekTo(scrubPendingSec);
        scrubSeekPending = false;
        // Real seek just committed -- let UI.cpp's tickPlaybackClock()
        // resume syncing posSec from the real decoder position again, now
        // that there's something real to sync FROM (see rotate()'s comment
        // on state.scrubPending).
        state.scrubPending = false;
    }
}

} // namespace InputRouter
