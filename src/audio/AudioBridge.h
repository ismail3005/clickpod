#pragma once

#include <Audio.h>
#include <SD.h>

// Bridges the UI layer's playback intent (play this mock Track, pause,
// change volume) to the real ESP32-audioI2S output. The UI's track
// metadata is placeholder data (see src/ui/Library.h / docs/SPEC.md
// section 10) with no real per-file mapping yet, so "playing a track"
// here actually plays the first playable audio file found on the card --
// you hear real audio out of the DAC, it just isn't guaranteed to be the
// specific mock track shown on screen until real library scanning exists.
namespace AudioBridge {

// Wires the Audio instance and I2S pins this bridge will drive. Call once
// from setup() after SD is mounted.
void begin(Audio &audio);

bool sdReady();

// Starts real playback of the first playable file found on the card, if
// any and if not already playing. Safe to call even with no SD card --
// just does nothing (the UI still runs its simulated position clock).
void playSomething();

void pauseResume();
void setVolumePercent(int pct); // 0-100, mapped to the DAC's 0-21 range

} // namespace AudioBridge
