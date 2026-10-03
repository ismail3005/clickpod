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
volatile uint32_t lastValidTransitionUs = 0;

// Standard full-step quadrature transition table: index = (old_AB<<2)|new_AB,
// value = +1/-1 for a valid single step, 0 for bounce/invalid transitions.
// NOTE: this table alone only rejects transitions between non-adjacent
// states -- it does NOT reject a rapid-fire stream of individually-VALID-
// looking steps caused by contact bounce/electrical noise right at a
// detent boundary, which is a well-known real failure mode for DIY/budget
// rotary encoders and matches "keeps moving on its own" exactly (not
// necessarily a wiring fault -- can happen on solid connections too, it's
// inherent to how these mechanical contacts bounce). Gated below with a
// minimum-time-between-counted-transitions check.
const int8_t kQuadTable[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

// A human physically turning this knob cannot produce valid full-step
// transitions faster than this even at a genuinely fast spin; contact
// bounce/electrical chatter typically repeats on the order of
// microseconds to a couple hundred microseconds, well under it.
// Deliberately generous (not hardware-measured against this exact
// encoder's real bounce characteristics) so a fast real spin is never
// mistaken for noise -- tune down further only if legitimate fast
// spins start feeling missed/sluggish after this.
constexpr uint32_t kMinTransitionIntervalUs = 1000; // 1ms

void IRAM_ATTR onEncoderChange() {
    uint8_t a = digitalRead(PIN_ANO_ENC_A);
    uint8_t b = digitalRead(PIN_ANO_ENC_B);
    uint8_t newState = (a << 1) | b;
    uint8_t index = (encoderLastState << 2) | newState;
    int8_t step = kQuadTable[index];
    encoderLastState = newState; // track real pin state every time, regardless of gating below

    if (step != 0) {
        uint32_t now = micros();
        if (now - lastValidTransitionUs >= kMinTransitionIntervalUs) {
            encoderDelta += step;
            lastValidTransitionUs = now;
        }
    }
}

} // namespace

void AnoInput::begin() {
    for (uint8_t i = 0; i < kButtonCount; i++) {
        // External 10k pull-ups on every ANO line (board has none onboard),
        // so plain INPUT here, not INPUT_PULLUP. (LEFT's resistor was
        // briefly borrowed during an earlier, abandoned attempt to test
        // GPIO0 for the backlight signal -- back in place since then.
        // The backlight now lives on GPIO4 (PIN_TFT_BL, see Pins.h),
        // driven with real PWM, needing no pull-up of its own either.
        // No INPUT_PULLUP exception needed here.)
        pinMode(kButtonPins[i], INPUT);
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
