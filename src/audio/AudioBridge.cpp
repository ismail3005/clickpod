#include "AudioBridge.h"

#include "../bt/BluetoothSource.h"
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

// Real fix for "two volume controls that don't agree" -- user reported
// the headphones' own volume buttons genuinely changed the audible
// level (visible in the serial log too) while the on-screen slider sat
// there as a completely separate control, stacking with it ("blast
// volume on both" / "reduce to zero on both but one would be
// undetectable"). Traced the real cause by reading Audio.cpp directly:
// Audio::playChunk() applies its own Gain() (the volume this function
// sets, 0-21) BEFORE calling audio_process_i2s() -- meaning whatever we
// hand to Bluetooth is ALREADY attenuated by this value. Separately,
// ESP32-A2DP's library-level set_volume()/volume_control() (0-127,
// AVRCP-synced with the connected device both directions -- see
// BluetoothSource.h) attenuates AGAIN on top of that whenever the
// connected device reports a volume change, which the library does
// automatically and unconditionally, not something app code can
// suppress. Two independent attenuation stages in series is exactly
// what was reported.
//
// Fixed by making this ONE stage active at a time instead of fighting
// each other: while Bluetooth is connected, this keeps the wired/I2S
// side pinned at max (unity -- no attenuation applied before
// audio_process_i2s() sees the samples) and drives the real, single
// volume control through BluetoothSource::setVolume() instead, scaled
// to its native 0-127 range -- which is also finer-grained than this
// function's own 0-21 DAC range, fixing the separately-reported
// coarseness/"on-screen can push past what the headphones' own buttons
// reach" complaint as a side effect of using the full native range
// instead of our own narrower one. Wired-only playback (BT not
// connected) is unchanged from before.
void setVolumePercent(int pct) {
    if (!audioPtr) return;
    pct = constrain(pct, 0, 100);
    if (BluetoothSource::isConnected()) {
        audioPtr->setVolume(21);
        BluetoothSource::setVolume((uint8_t)map(pct, 0, 100, 0, 127));
    } else {
        audioPtr->setVolume(map(pct, 0, 100, 0, 21));
    }
}

bool seekTo(uint16_t sec) {
    if (!audioPtr || !playing) return false;
    // Deliberately NOT bracketed with an internal pause/resume (tried in an
    // earlier round, reverted -- see CLAUDE.md's seventeenth hardware bug
    // writeup). User explicitly wants playback to keep running
    // uninterrupted through a scrub gesture and only actually relocate
    // once, at the very end -- any audible pause during that one real
    // commit, even brief, defeats the point. The false-syncword-acceptance
    // bug that made seeking-while-playing genuinely unsafe is fixed at the
    // decoder level instead (both the seek-path and mid-stream resync now
    // CRC-8-verify a candidate frame header before trusting it -- see the
    // fourteenth/sixteenth hardware bug writeups), which is where this
    // belongs: fixing the decoder's own resync robustness, not avoiding
    // ever calling it live.
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

// ESP32-audioI2S's own documented extension point (Audio.h: "extern weak
// void audio_process_i2s(...); // record audiodata or send via BT") --
// called by Audio::playChunk() with every decoded PCM buffer right
// before it would go to I2S, already 44.1kHz 16-bit stereo interleaved
// (the library upmixes mono and widens 8-bit to 16-bit before this call).
// Real routing for Bluetooth output, replacing the old 440Hz test tone:
// whenever BT is actually connected, feed these exact samples into
// BluetoothSource's ring buffer and skip the I2S write entirely --
// matches this project's "wired and BT are mutually exclusive output
// paths" design (docs/SPEC.md section 7). When BT isn't connected,
// behavior is unchanged: normal wired I2S output.
void audio_process_i2s(int16_t *outBuff, uint16_t validSamples, uint8_t bitsPerSample, uint8_t channels,
                        bool *continueI2S) {
    if (BluetoothSource::isConnected()) {
        // validSamples is a FRAME count (one unit per channel-interleaved
        // sample group), not a total int16-word count -- confirmed by
        // reading Audio.cpp's own i2s_write() call right after this hook
        // fires: it sizes the write as
        // `validSamples * (bitsPerSample/8) * channels` bytes. This code
        // previously assumed validSamples was already a total 16-bit-word
        // count and used `validSamples * 2`, which for the normal 16-bit
        // stereo case is exactly HALF the real buffer -- silently feeding
        // only the first half of every decoded chunk into the BT ring
        // buffer while the second half got discarded/overwritten by the
        // next decode pass. That's a real, structural data-corruption bug,
        // not a buffering/timing issue -- it's the actual root cause of
        // "garbled and noisy" BT audio (the bigger ring buffer + backpressure
        // fix from the previous round was a correct improvement on its own
        // but couldn't have fixed this, since the data fed in was already
        // wrong before it ever reached the ring buffer).
        size_t byteCount = (size_t)validSamples * (bitsPerSample / 8) * channels;
        BluetoothSource::feedPcm(reinterpret_cast<const uint8_t *>(outBuff), byteCount);
        *continueI2S = false;
    } else {
        *continueI2S = true;
    }
}
