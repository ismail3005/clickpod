#pragma once

#include <TFT_eSPI.h>

// TFT_eSPI rendering for every screen, ported from the browser simulator's
// render()/renderX() functions. Screens::render() is the equivalent of the
// simulator's top-level render(): it looks at `state` and draws whichever
// screen is current. Redraws are full-screen and only happen when
// state.dirty is set (see UI::tick()) -- this is a button-driven UI, not
// an animation, so there's no need to redraw every loop iteration.
namespace Screens {

void begin(TFT_eSPI &tft);
void render(); // draws the current state.mode's screen if state.dirty, then clears dirty

} // namespace Screens
