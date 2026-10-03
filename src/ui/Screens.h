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

// Sends the ILI9341's own WRDISBV/WRCTRLD brightness commands over SPI --
// CONFIRMED a no-op on this specific Waveshare module (its backlight
// bypasses the controller entirely, see CLAUDE.md's backlight-hardware
// section), kept harmlessly in case a future board swap ever uses a
// module where it isn't. No GPIO PWM right now -- both GPIO0 and GPIO12
// failed real-hardware testing for the backlight signal; parked until a
// non-strapping pin is freed up instead. percent is 0-100, clamped.
void applyBrightness(int percent);

// Commands the ILI9341 into DISPLAY OFF + SLEEP IN (real controller
// commands, same writecommand() mechanism applyBrightness() already
// uses, not guessed) right before a real Power Off (deep sleep) --
// see MenuEngine.cpp's openShutdownConfirm(). The backlight itself
// stays lit regardless (hardwired to 3.3V, no GPIO control -- see
// CLAUDE.md), but this blanks the panel's own output instead of
// leaving whatever was last drawn on screen once the CPU stops
// actively refreshing it.
void prepareForDeepSleep();

} // namespace Screens
