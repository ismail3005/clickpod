#pragma once

#include <TFT_eSPI.h>

// TFT_eSPI rendering for every screen, ported from the browser simulator's
// render()/renderX() functions. Screens::render() is the equivalent of the
// simulator's top-level render(): it looks at `state` and draws whichever
// screen is current. Full redraws only happen when state.dirty is set
// (menu navigation, mode/track changes) -- this is a button-driven UI, not
// an animation. state.progressDirty is a lighter-weight signal for the
// once-a-second Now Playing position tick: only the progress bar/time
// strip redraws, not the whole screen, so playback doesn't full-screen-
// flicker every second (see UI.cpp's tickPlaybackClock).
namespace Screens {

void begin(TFT_eSPI &tft);
void render(); // draws per state.dirty/progressDirty, then clears whichever fired

// Draws msg immediately, bypassing the normal dirty-flag render() path --
// for a blocking call (e.g. a manual library rescan) that needs on-screen
// feedback BEFORE it blocks, not after the fact.
void showBusyMessage(const String &msg);

} // namespace Screens
