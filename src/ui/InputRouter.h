#pragma once

#include "../input/AnoInput.h"

// Translates ANO input events into state transitions, ported function-for-
// function from the simulator's handleTap/handleLongPress/handleRelease/
// handleDoubleTap/togglePower/rotate. Call InputRouter::update() once per
// loop() iteration after AnoInput::update(); it drains AnoInput's event
// flags itself.
namespace InputRouter {

void update();

} // namespace InputRouter
