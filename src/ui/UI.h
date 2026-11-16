#pragma once

#include <TFT_eSPI.h>

// Top-level UI glue: owns startup, the per-loop tick (input dispatch,
// watching the playing stream, the playback clock) and triggering a
// redraw when state changes. This is the firmware port of the browser
// simulator's boot sequence + setInterval-driven playback clock +
// render() calls. Real audio output is wired up separately by the caller
// via AudioBridge::begin() -- see main.cpp.
namespace UI {

void begin(TFT_eSPI &tft);
void update(); // call once per loop() iteration, after AnoInput::update()

} // namespace UI
