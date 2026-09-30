#include "AudioBridge.h"

#include "../config/Pins.h"
#include "../ui/Util.h"

namespace AudioBridge {
namespace {

Audio *audioPtr = nullptr;
bool sdOk = false;
bool playing = false;

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
