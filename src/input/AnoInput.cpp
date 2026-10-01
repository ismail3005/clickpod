#include "AnoInput.h"
#include <Arduino.h>
#include <Bounce2.h>

#include "../config/Pins.h"

namespace {

constexpr uint32_t kLongPressMs = 700;
constexpr uint32_t kDoubleTapWindowMs = 350;
constexpr uint32_t kDebounceMs = 15;
constexpr uint8_t kButtonCount = static_cast<uint8_t>(AnoButton::COUNT);

constexpr uint8_t kButtonPins[kButtonCount] = {
    PIN_ANO_BTN_UP, PIN_ANO_BTN_DOWN, PIN_ANO_BTN_LEFT, PIN_ANO_BTN_RIGHT, PIN_ANO_BTN_CENTER,
};

Bounce debouncers[kButtonCount];

bool tapped[kButtonCount] = {};
bool longPressed[kButtonCount] = {};
uint32_t pressStartMs[kButtonCount] = {};
bool longPressFired[kButtonCount] = {};

// CENTER-specific double-tap tracking (spec 5.3): a release starts a short
// timer; a second release within the window fires double-tap immediately,
// otherwise single-tap fires once the window elapses with nothing else.
bool centerAwaitingSecondTap = false;
uint32_t centerFirstTapReleaseMs = 0;
bool centerDoubleTapEvent = false;

// Quadrature encoder decode, interrupt-driven so it doesn't depend on loop()
// timing (which also has audio.loop() competing for CPU time).
volatile int16_t encoderDelta = 0;
volatile uint8_t encoderLastState = 0;

// Standard full-step quadrature transition table: index = (old_AB<<2)|new_AB,
// value = +1/-1 for a valid single step, 0 for bounce/invalid transitions.
const int8_t kQuadTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

void IRAM_ATTR onEncoderChange() {
    uint8_t a = digitalRead(PIN_ANO_ENC_A);
    uint8_t b = digitalRead(PIN_ANO_ENC_B);
    uint8_t newState = (a << 1) | b;
    uint8_t index = (encoderLastState << 2) | newState;
    encoderDelta += kQuadTable[index];
    encoderLastState = newState;
}

} // namespace

void AnoInput::begin() {
    for (uint8_t i = 0; i < kButtonCount; i++) {
        // External 10k pull-ups on every ANO line (board has none onboard),
        // so plain INPUT here, not INPUT_PULLUP.
        //
        // TEMPORARY EXCEPTION: LEFT's external pull-up was borrowed to test
        // GPIO0 (backlight -- see Pins.h/CLAUDE.md's backlight-hardware
        // writeup) without an extra resistor on hand. GPIO13 (LEFT) is a
        // regular GPIO with real internal pull-up capability (unlike
        // DOWN/RIGHT/CENTER, which are input-only pins with NO internal
        // pull option at all -- those could never be borrowed from this
        // way), so INPUT_PULLUP compensates in software for the missing
        // external resistor. Safe to leave this in place permanently even
        // after a real resistor goes back on LEFT -- redundant pull-ups in
        // parallel don't hurt anything -- but if LEFT feels any less
        // reliable than the other buttons, that's the first thing to
        // check/revert.
        pinMode(kButtonPins[i], kButtonPins[i] == PIN_ANO_BTN_LEFT ? INPUT_PULLUP : INPUT);
        debouncers[i].attach(kButtonPins[i]);
        debouncers[i].interval(kDebounceMs);
    }

    pinMode(PIN_ANO_ENC_A, INPUT);
    pinMode(PIN_ANO_ENC_B, INPUT);
    encoderLastState = (digitalRead(PIN_ANO_ENC_A) << 1) | digitalRead(PIN_ANO_ENC_B);
    attachInterrupt(digitalPinToInterrupt(PIN_ANO_ENC_A), onEncoderChange, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ANO_ENC_B), onEncoderChange, CHANGE);
}

void AnoInput::update() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < kButtonCount; i++) {
        tapped[i] = false;
        longPressed[i] = false;

        debouncers[i].update();

        if (debouncers[i].fell()) {
            pressStartMs[i] = now;
            longPressFired[i] = false;
        }

        bool held = debouncers[i].read() == LOW; // active-low, external pull-up
        if (held && !longPressFired[i] && (now - pressStartMs[i] >= kLongPressMs)) {
            longPressed[i] = true;
            longPressFired[i] = true;
        }

        if (debouncers[i].rose()) {
            // A long-press already fired for this press -- don't also treat
            // the release as a tap (spec 5.3: no spurious tap after a hold).
            if (!longPressFired[i]) {
                if (static_cast<AnoButton>(i) == AnoButton::CENTER) {
                    if (centerAwaitingSecondTap &&
                        (now - centerFirstTapReleaseMs <= kDoubleTapWindowMs)) {
                        centerDoubleTapEvent = true;
                        centerAwaitingSecondTap = false;
                    } else {
                        centerAwaitingSecondTap = true;
                        centerFirstTapReleaseMs = now;
                    }
                } else {
                    tapped[i] = true;
                }
            }
        }
    }

    if (centerAwaitingSecondTap && (now - centerFirstTapReleaseMs > kDoubleTapWindowMs)) {
        tapped[static_cast<uint8_t>(AnoButton::CENTER)] = true;
        centerAwaitingSecondTap = false;
    }
}

int16_t AnoInput::takeEncoderDelta() {
    noInterrupts();
    int16_t d = encoderDelta;
    encoderDelta = 0;
    interrupts();
    return d;
}

bool AnoInput::wasTapped(AnoButton button) {
    return tapped[static_cast<uint8_t>(button)];
}

bool AnoInput::wasLongPressed(AnoButton button) {
    return longPressed[static_cast<uint8_t>(button)];
}

bool AnoInput::isHeld(AnoButton button) {
    return debouncers[static_cast<uint8_t>(button)].read() == LOW;
}

bool AnoInput::centerWasDoubleTapped() {
    if (centerDoubleTapEvent) {
        centerDoubleTapEvent = false;
        return true;
    }
    return false;
}
