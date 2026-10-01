#include "AudioBridge.h"

#include "../config/Pins.h"
#include "../ui/Util.h"

namespace AudioBridge {
namespace {

Audio *audioPtr = nullptr;
bool sdOk = false;
bool playing = false;

// Guards against calling audioPtr->connecttoFS() again before a previous
// call has had any chance to settle. ESP32-audioI2S isn't built to tolerate
// being told to open a new stream while it's still mid-setup from the last
// one -- a real-hardware crash (Guru Meditation LoadProhibited, garbage
// pointer deref) was traced to exactly this: serial logs showed
// playSomething() being invoked roughly 20 times back-to-back, in well
// under a second, for the track a user was navigating to (garbled/
// truncated log lines from the UART contention this caused, not literal
// distinct per-call text) -- not a single cleanly-ordered call like every
// other path assumes. Whatever UI-level trigger produced that storm (not
// conclusively identified this round -- a genuinely-bouncing button press,
// or several menu rows firing in quick succession, are both plausible),
// playSomething() itself had no defense against it, and this library isn't
// safe to hand that to. A real human action (even a fast deliberate
// double-skip) can't plausibly need a second track-start inside 150ms, so
// this is a correctness guard, not a feature limitation.
constexpr uint32_t kMinMsBetweenPlaySomething = 150;
uint32_t lastPlaySomethingMs = 0;

// Recursively searches for the first playable, actually-openable audio file
// on the card -- see main.cpp's original bring-up comment for why paths are
// threaded through the recursion and why SD.exists() re-checks each
// candidate (a file can list but still fail to open by that exact path).
bool findFirstAudioFile(File dir, const String &dirPath, String &outPath) {
    while (File entry = dir.openNextFile()) {
        String path = dirPath + "/" + entry.name();

        if (entry.isDirectory()) {
            bool found = findFirstAudioFile(entry, path, outPath);
            entry.close();
            if (found) return true;
        } else {
            if (hasAudioExtension(path) && SD.exists(path)) {
                outPath = path;
                entry.close();
                return true;
            }
            entry.close();
        }
    }
    return false;
}

} // namespace

void begin(Audio &audio) {
    audioPtr = &audio;
    sdOk = SD.cardType() != CARD_NONE;
    if (sdOk) {
        audioPtr->setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
    }
}

bool sdReady() { return sdOk; }

void playSomething(const String &path) {
    if (!audioPtr || !sdOk) return;

    uint32_t now = millis();
    if (now - lastPlaySomethingMs < kMinMsBetweenPlaySomething) {
        Serial.println(F("[audio] ignoring playSomething() -- called again too soon after the "
                          "last one (see AudioBridge.cpp's kMinMsBetweenPlaySomething comment)"));
        return;
    }
    lastPlaySomethingMs = now;

    if (path.length() > 0) {
        Serial.printf("[audio] playing: %s\n", path.c_str());
        audioPtr->connecttoFS(SD, path.c_str());
        playing = true;
        return;
    }

    // No real path known for this track (placeholder/mock data) -- fall
    // back to whatever's first on the card, so DAC output is still real.
    String trackPath;
    File root = SD.open("/");
    bool found = findFirstAudioFile(root, "", trackPath);
    root.close();

    if (!found) {
        Serial.println(F("[audio] no playable file found on card"));
        return;
    }

    Serial.printf("[audio] playing (fallback, no track path known): %s\n", trackPath.c_str());
    audioPtr->connecttoFS(SD, trackPath.c_str());
    playing = true;
}

void pauseResume() {
    if (!audioPtr || !playing) return;
    audioPtr->pauseResume();
}

void setVolumePercent(int pct) {
    if (!audioPtr) return;
    pct = constrain(pct, 0, 100);
    audioPtr->setVolume(map(pct, 0, 100, 0, 21));
}

bool seekTo(uint16_t sec) {
    if (!audioPtr || !playing) return false;
    return audioPtr->setAudioPlayPosition(sec);
}

uint32_t currentTimeSec() {
    if (!audioPtr || !playing) return 0;
    return audioPtr->getAudioCurrentTime();
}

bool isRunning() {
    if (!audioPtr || !playing) return false;
    return audioPtr->isRunning();
}

} // namespace AudioBridge
