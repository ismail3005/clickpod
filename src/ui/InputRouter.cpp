#include "InputRouter.h"

#include "../audio/AudioBridge.h"
#include "../state/AppState.h"
#include "MenuEngine.h"

namespace InputRouter {
namespace {

constexpr uint32_t kFastScrollRepeatMs = 120; // matches the simulator's FAST_SCROLL_REPEAT_MS

AnoButton repeatButton = AnoButton::COUNT; // COUNT = "no button currently fast-scrolling"
uint32_t repeatLastMs = 0;

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
        state.now.posSec = constrain(state.now.posSec + dir * 3, 0.0f, (float)state.now.durSec);
        state.dirty = true;
    } else if (state.mode == AppMode::QUEUE) {
        MenuEngine::moveQueueSelection(dir > 0 ? 1 : -1);
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
                MenuEngine::exitBluetooth();
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
        if (btn == AnoButton::RIGHT) { state.queueGrabbed = !state.queueGrabbed; state.dirty = true; }
        else if (state.queueGrabbed) {
            if (btn == AnoButton::UP) MenuEngine::moveGrabbedQueueItem(-1);
            else if (btn == AnoButton::DOWN) MenuEngine::moveGrabbedQueueItem(1);
        }
        else if (btn == AnoButton::UP) MenuEngine::moveQueueSelection(-1);
        else if (btn == AnoButton::DOWN) MenuEngine::moveQueueSelection(1);
        else if (btn == AnoButton::LEFT) { state.mode = AppMode::NOW_PLAYING; state.queueGrabbed = false; state.dirty = true; }
        else if (btn == AnoButton::CENTER) MenuEngine::playFromQueueIndex(state.queueSelected);
    }
}

void handleLongPress(AnoButton btn) {
    if (btn == AnoButton::CENTER) { togglePower(); return; }
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
        else if (btn == AnoButton::UP) { state.mode = AppMode::QUEUE; state.queueSelected = 0; state.queueGrabbed = false; state.dirty = true; }
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
        else if (btn == AnoButton::LEFT && !state.queue.empty() && !state.queueGrabbed) {
            MenuEngine::openTrackMenu(state.queue[state.queueSelected]);
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
}

} // namespace InputRouter
