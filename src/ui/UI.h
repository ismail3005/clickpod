#pragma once

#include <TFT_eSPI.h>

// Top-level UI glue: owns startup, the per-loop tick (input dispatch +
// simulated playback clock), and triggering a redraw when state changes.
// This is the firmware port of the browser simulator's boot sequence +
// setInterval-driven playback clock + render() calls. Real audio output is
// wired up separately by the caller via AudioBridge::begin() -- see
// main.cpp -- since not every build mode wants it (BT bring-up mode does
// not).
namespace UI {

// How long tickPlaybackClock() gives a real track to actually start
// producing audio (via AudioBridge::isRunning()) before treating it as a
// decode failure and auto-skipping -- see AppState.h's NowPlaying
// comment. Public (not file-local to UI.cpp) so MenuEngine.cpp's
// setNowPlaying() can backdate NowPlaying::startedAtMs by exactly this
// much for a track it already knows can't play (e.g. 24-bit FLAC, see
// FlacMeta::StreamInfo::bitsPerSample) -- reuses the same, already-
// tested, non-recursive skip path on the very next tick instead of a
// separate special-cased one.
constexpr uint32_t kPlaybackStartGraceMs = 3000;

void begin(TFT_eSPI &tft);
void update(); // call once per loop() iteration, after AnoInput::update()

} // namespace UI
