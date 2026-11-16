#pragma once

#include <Audio.h>
#include <SD.h>

// Bridges the UI layer's playback intent (play this Track, pause, seek,
// change volume) to the real ESP32-audioI2S output, and is the ONLY place
// that talks to the Audio object after setup().
//
// Threading model worth knowing before touching this (confirmed from the
// library source, see CLAUDE.md): decoding and output run on the library's
// OWN FreeRTOS task. audio.loop() -- called from the Arduino loop task --
// only reads the file from SD into the decoder's input buffer. So every SD
// access stays on the loop task, and the loop task can block for a while
// without stalling audio, as long as that buffer doesn't run dry: see
// pump().
namespace AudioBridge {

// What the stream is doing, from the app's point of view.
enum class Status : uint8_t {
    Idle,     // nothing has been started
    Starting, // a start was requested too soon after the last one and is queued (see tick())
    Playing,
    Paused,
    Ended,    // was playing and stopped by itself: end of file, or the decoder gave up
    Failed,   // the file could not be opened/started at all
};

// Wires the Audio instance and I2S pins this bridge will drive. Call once
// from setup() after SD is mounted.
void begin(Audio &audio);

// Starts real playback. If path is non-empty, plays that exact SD file;
// otherwise (placeholder/mock tracks) falls back to the first playable
// file found on the card. Safe to call with no SD card. Check status()
// afterwards -- Failed means the file could not be started.
void playSomething(const String &path = "");

// Call once per loop() iteration: starts a queued (Status::Starting) track
// once the minimum spacing between two real starts has elapsed.
void tick();

// Keeps the decoder's input buffer fed (one audio.loop() pass). Call from
// inside any long-running main-loop work -- a JPEG decode, a big SD read, a
// library rescan -- so playback doesn't run dry while the loop task is
// busy. Cheap when there is nothing to do, safe to call anywhere on the
// loop task, a no-op if re-entered.
void pump();

Status status();
bool isRunning(); // Status::Playing

// True once it is a good moment for the loop task to do something slow
// (the decoder's input buffer is filled, or nothing is playing).
bool streamSettled();

void pauseResume(); // toggles Playing <-> Paused; no-op in any other state

// Silences whatever is playing and forgets it (status() goes to Idle).
// The stream cannot be resumed afterwards; the next playSomething()
// starts clean. Used when the UI moves on to a track that will not be
// played (unsupported file, failure breaker).
void halt();

void setVolumePercent(int pct); // 0-100; see the .cpp for the wired/Bluetooth split

// Real seek/position (ESP32-audioI2S: setAudioPlayPosition(),
// getAudioCurrentTime(), getAudioFileDuration()). The seek is carried out
// by the next audio.loop() pass, not synchronously -- until the decoder has
// refilled and decoded its first frame at the new spot, currentTimeSec()
// still reports the old position (see AppState::seekSettleUntilMs).
bool seekTo(uint16_t sec);
uint32_t currentTimeSec();
uint32_t durationSec(); // the decoder's own figure; 0 until it knows

} // namespace AudioBridge
