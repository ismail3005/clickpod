#include "AudioBridge.h"

#include "../bt/BluetoothSource.h"
#include "../config/Pins.h"
#include "../ui/Util.h"

namespace AudioBridge {
namespace {

Audio *audioPtr = nullptr;
bool sdOk = false;

// A file has been handed to the decoder and not yet given up on. Stays
// true through a natural end of file (status() reports Ended from the
// library's own running flag) until the next start or halt().
bool loaded = false;
bool paused = false;
bool startFailed = false;

// ESP32-audioI2S fills its whole input buffer from SD before it starts
// decoding a file, and again after every seek. Its PSRAM default is
// ~640KB, i.e. the better part of a second of blocking SD reads before
// the first sound of every track and after every scrub. 256KB total
// (~190KB usable after the library's 64KB wrap-around reserve -- still
// over a second of audio at the highest bitrate 16-bit/44.1kHz FLAC can
// reach) cuts that to roughly a third. The cost is less slack for the
// loop task to be busy without feeding it, which is what pump() is for.
constexpr int kInputBufferBytes = 256 * 1024;

// Two real starts closer together than this are never a deliberate user
// action. The second one is not dropped (the UI has already moved on to
// that track -- dropping it would leave the old track playing under the
// new track's screen); it is queued and started by tick().
constexpr uint32_t kMinMsBetweenStarts = 150;
uint32_t lastStartMs = 0;
bool startQueued = false;
String queuedPath;

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

void startNow(const String &path) {
    String trackPath = path;
    if (trackPath.length() == 0) {
        // No real path known for this track (placeholder/mock data) -- fall
        // back to whatever's first on the card, so DAC output is still real.
        File root = SD.open("/");
        bool found = root && findFirstAudioFile(root, "", trackPath);
        if (root) root.close();
        if (!found) {
            Serial.println(F("[audio] no playable file found on card"));
            loaded = false;
            startFailed = true;
            return;
        }
    }

    // Whatever is still queued for Bluetooth belongs to the previous track.
    BluetoothSource::flushPcm();

    Serial.printf("[audio] playing: %s\n", trackPath.c_str());
    lastStartMs = millis();
    bool ok = audioPtr->connecttoFS(SD, trackPath.c_str());
    loaded = ok;
    paused = false;
    startFailed = !ok;
    if (!ok) Serial.printf("[audio] could not start: %s\n", trackPath.c_str());
}

} // namespace

void begin(Audio &audio) {
    audioPtr = &audio;
    sdOk = SD.cardType() != CARD_NONE;
    // Must happen before the first connecttoFS() -- the library allocates
    // its input buffer lazily on first use and refuses a resize afterwards.
    audioPtr->setBufsize(-1, kInputBufferBytes);
    if (sdOk) {
        audioPtr->setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
    }
}

void playSomething(const String &path) {
    startQueued = false;
    if (!audioPtr || !sdOk) {
        loaded = false;
        startFailed = true;
        return;
    }
    if (millis() - lastStartMs < kMinMsBetweenStarts) {
        queuedPath = path;
        startQueued = true;
        return;
    }
    startNow(path);
}

void tick() {
    if (!startQueued || millis() - lastStartMs < kMinMsBetweenStarts) return;
    startQueued = false;
    startNow(queuedPath);
}

void pump() {
    static bool busy = false;
    if (busy || !audioPtr) return;
    busy = true;
    audioPtr->loop();
    busy = false;
}

Status status() {
    if (startQueued) return Status::Starting;
    if (startFailed) return Status::Failed;
    if (!loaded || !audioPtr) return Status::Idle;
    if (paused) return Status::Paused;
    return audioPtr->isRunning() ? Status::Playing : Status::Ended;
}

bool isRunning() { return status() == Status::Playing; }

bool streamSettled() {
    if (status() != Status::Playing) return status() != Status::Starting;
    // The library starts decoding once its buffer is all but full, so
    // "90% full" means the first sound is out (or about to be); a file
    // smaller than the buffer never gets there and is caught by the
    // decoder's own position instead.
    return audioPtr->inBufferFilled() * 10 >= audioPtr->inBufferSize() * 9 || audioPtr->getAudioCurrentTime() >= 1;
}

void pauseResume() {
    // Only ever toggles a stream that is really there. The library's own
    // pauseResume() blindly flips its running flag, so calling it on a
    // stream that already ended would mark a closed file as "running".
    Status st = status();
    if (st == Status::Playing) {
        audioPtr->pauseResume();
        paused = true;
    } else if (st == Status::Paused) {
        audioPtr->pauseResume();
        paused = false;
    }
}

void halt() {
    startQueued = false;
    startFailed = false;
    if (status() == Status::Playing) audioPtr->pauseResume(); // the library's one race-free way to stop output
    loaded = false;
    paused = false;
    BluetoothSource::flushPcm();
}

// Wired and Bluetooth volume are two different attenuation stages (see
// CLAUDE.md's thirty-third hardware bug): the decoder's own gain (0-21),
// applied before the samples reach audio_process_i2s() below, and
// ESP32-A2DP's AVRCP-synced volume (0-127), applied on top of whatever we
// feed it and changed by the headphones' own buttons. Only ONE is ever
// active: while Bluetooth is connected the decoder's gain is pinned at
// unity and the on-screen volume drives the Bluetooth one (finer-grained,
// and in step with the headphones' own readout); otherwise the decoder's
// gain is the volume control. main.cpp re-applies this on every
// connect/disconnect.
void setVolumePercent(int pct) {
    if (!audioPtr) return;
    pct = constrain(pct, 0, 100);
    if (BluetoothSource::isConnected()) {
        audioPtr->setVolume(21);
        BluetoothSource::setVolume((uint8_t)::map(pct, 0, 100, 0, 127));
    } else {
        audioPtr->setVolume(::map(pct, 0, 100, 0, 21));
    }
}

bool seekTo(uint16_t sec) {
    Status st = status();
    if (st != Status::Playing && st != Status::Paused) return false;
    // Deliberately NOT bracketed with a pause/resume (tried once, reverted
    // -- user wants playback to carry on through a scrub and relocate
    // once). The seek itself is made safe inside the library fork instead.
    BluetoothSource::flushPcm();
    return audioPtr->setAudioPlayPosition(sec);
}

uint32_t currentTimeSec() {
    if (!loaded || !audioPtr) return 0;
    return audioPtr->getAudioCurrentTime();
}

uint32_t durationSec() {
    if (!loaded || !audioPtr) return 0;
    return audioPtr->getAudioFileDuration();
}

} // namespace AudioBridge

// ESP32-audioI2S's own documented extension point (Audio.h: "extern weak
// void audio_process_i2s(...); // record audiodata or send via BT") --
// called on the library's decode task with every decoded PCM buffer right
// before it would go to I2S, already 44.1kHz 16-bit stereo interleaved
// (the library upmixes mono and widens 8-bit before this call). While
// Bluetooth is connected the samples go to BluetoothSource's ring buffer
// and the I2S write is skipped -- wired and Bluetooth are mutually
// exclusive outputs (docs/SPEC.md section 7), decided afresh for every
// buffer. Otherwise: normal wired output.
void audio_process_i2s(int16_t *outBuff, uint16_t validSamples, uint8_t bitsPerSample, uint8_t channels,
                        bool *continueI2S) {
    if (BluetoothSource::isConnected()) {
        // validSamples is a FRAME count (one unit per channel-interleaved
        // sample group), not a count of int16 words -- the library's own
        // i2s_write() right after this hook sizes its write the same way.
        // (Treating it as words fed Bluetooth half of every buffer: the
        // twenty-eighth hardware bug.)
        size_t byteCount = (size_t)validSamples * (bitsPerSample / 8) * channels;
        BluetoothSource::feedPcm(reinterpret_cast<const uint8_t *>(outBuff), byteCount);
        *continueI2S = false;
    } else {
        *continueI2S = true;
    }
}
