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

// Real backlight brightness control: drives GPIO4 (PIN_TFT_BL) with PWM
// into the display module's own onboard backlight-switching transistor.
// Also still sends the ILI9341's own WRDISBV/WRCTRLD commands over SPI,
// confirmed a no-op on this specific module (its backlight bypasses the
// controller's internal PWM entirely) but harmless, kept in case a
// future board swap ever uses a module that DOES route brightness
// through the controller. percent is 0-100, clamped. Settings' own
// Brightness slider and AOD's dim level (InputRouter.cpp's
// togglePower()) both funnel through this one function.
void applyBrightness(int percent);

// Commands the ILI9341 into DISPLAY OFF + SLEEP IN (real controller
// commands, same writecommand() mechanism applyBrightness() already
// uses) and drives the backlight to 0% right before a real Power Off
// (deep sleep) -- see MenuEngine.cpp's openShutdownConfirm(), which
// also holds PIN_TFT_BL low through the sleep itself so it can't float
// back on. Unlike before GPIO4 existed, this now genuinely blanks the
// panel AND kills the backlight -- Power Off should look properly dark.
void prepareForDeepSleep();

} // namespace Screens
