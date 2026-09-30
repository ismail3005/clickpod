#pragma once

#include <Audio.h>
#include <SD.h>

// Bridges the UI layer's playback intent (play this Track, pause, change
// volume) to the real ESP32-audioI2S output. When a track came from
// Library::scanFromSd() it carries a real SD path and that exact file
// plays; for placeholder/mock tracks with no path (Track::path empty),
// falls back to playing the first playable file found on the card, so
// DAC output is still real even without a per-track mapping.
namespace AudioBridge {

// Wires the Audio instance and I2S pins this bridge will drive. Call once
// from setup() after SD is mounted.
void begin(Audio &audio);

bool sdReady();

// Starts real playback. If path is non-empty, plays that exact SD file;
// otherwise falls back to the first playable file found on the card. Safe
// to call even with no SD card -- just does nothing (the UI still runs
// its simulated position clock).
void playSomething(const String &path = "");

void pauseResume();
void setVolumePercent(int pct); // 0-100, mapped to the DAC's 0-21 range

} // namespace AudioBridge
