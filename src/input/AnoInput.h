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

// Samples the buttons. Call every loop() iteration -- and it is fine (and
// useful) to call it more often: the buttons are polled, not
// interrupt-driven, so a press that both starts and ends while the main
// loop is busy with something long would otherwise never be seen. Long
// operations call this as they go; the events it detects are latched
// until read (see below).
void update();

// Encoder detents accumulated since the last call, then reset to 0.
// Positive = clockwise, negative = counter-clockwise.
int16_t takeEncoderDelta();

// True ONCE per short press, after the button is released (below the
// long-press threshold): latched by update(), cleared by this call. For
// CENTER, this only fires after the double-tap window has elapsed with no
// second tap -- see centerWasDoubleTapped() for that case instead.
bool wasTapped(AnoButton button);

// True ONCE per hold, from the moment a held button crosses the
// long-press threshold (on hold -- not on release). Latched like
// wasTapped().
bool wasLongPressed(AnoButton button);

// True if anything is waiting to be read (a tap, a long-press, a
// double-tap, encoder movement). Lets slow background work -- the cover
// art decode -- get out of the way of the user instead of making them
// wait for it.
bool hasPendingEvent();

// Throws away everything latched so far (used while the boot splash is
// up, so buttons pressed during boot don't fire at the first screen).
void discardEvents();

// True while the button is currently held down, past debounce. Useful for
// continuous behavior while held (e.g. MENU fast-scroll on UP/DOWN).
bool isHeld(AnoButton button);

// CENTER only: true once when a second tap landed within the double-tap
// window of the first release.
bool centerWasDoubleTapped();

} // namespace AnoInput
