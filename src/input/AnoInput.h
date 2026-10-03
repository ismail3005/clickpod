#pragma once
#include <stdint.h>

// Raw ANO input handling: quadrature encoder decode + the 5 navigation
// buttons, including the tap/double-tap/long-press state machine for
// CENTER required by docs/SPEC.md section 5.3. This module only reports
// low-level input events -- deciding what each event MEANS in a given UI
// mode (section 5.1/5.2) is the menu/screen code's job, once that exists.

enum class AnoButton : uint8_t { UP, DOWN, LEFT, RIGHT, CENTER, COUNT };

namespace AnoInput {

void begin();
void update(); // call every loop() iteration, cheap

// Encoder detents accumulated since the last call, then reset to 0.
// Positive = clockwise, negative = counter-clockwise.
int16_t takeEncoderDelta();

// True for exactly one update() call when the button was just released
// after a short press (below the long-press threshold). For CENTER, this
// only fires after the double-tap window has elapsed with no second tap --
// see centerWasDoubleTapped() for that case instead.
bool wasTapped(AnoButton button);

// True for exactly one update() call, the moment a held button crosses
// the long-press threshold (fires once, on hold -- not on release).
bool wasLongPressed(AnoButton button);

// True while the button is currently held down, past debounce. Useful for
// continuous behavior while held (e.g. MENU fast-scroll on UP/DOWN).
bool isHeld(AnoButton button);

// CENTER only: true for exactly one update() call when a second tap
// landed within the double-tap window of the first release.
bool centerWasDoubleTapped();

} // namespace AnoInput
