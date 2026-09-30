#include "UI.h"

#include "../state/AppState.h"
#include "InputRouter.h"
#include "MenuEngine.h"
#include "Screens.h"

namespace UI {
namespace {

constexpr uint32_t kBootMs = 1100;   // matches the simulator's boot->MENU timeout
constexpr uint32_t kClockMs = 500;   // matches the simulator's playback-clock setInterval

uint32_t bootAt = 0;
uint32_t lastClockMs = 0;
bool booted = false;

void tickPlaybackClock() {
    uint32_t now = millis();
    if (now - lastClockMs < kClockMs) return;
    lastClockMs = now;

    if (state.mode == AppMode::OFF) return;
    if (!state.now.playing || !state.now.hasTrack) return;

    state.now.posSec += kClockMs / 1000.0f;
    if (state.now.posSec >= state.now.durSec) {
        MenuEngine::playNextInQueue();
    }
    state.dirty = true;
}

} // namespace

void begin(TFT_eSPI &tft) {
    Screens::begin(tft);

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
