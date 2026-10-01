#include "BluetoothSource.h"
#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <math.h>

#include "../net/RadioLock.h"

namespace {

BluetoothA2DPSource a2dpSource;
bool running = false;

constexpr float kSampleRate = 44100.0f;
constexpr float kToneHz = 440.0f;
constexpr float kPhaseIncrement = 2.0f * PI * kToneHz / kSampleRate;
float phase = 0.0f;

// Raw callback: fill `data` with interleaved 16-bit signed stereo PCM at
// 44.1kHz, the format ESP32-A2DP's source expects. byteCount is how many
// bytes the library wants; return how many were actually written.
int32_t provideTestTone(uint8_t *data, int32_t byteCount) {
    int16_t *samples = reinterpret_cast<int16_t *>(data);
    int32_t frameCount = byteCount / 4; // 4 bytes per stereo frame (2ch x 16-bit)

    for (int32_t i = 0; i < frameCount; i++) {
        int16_t sample = static_cast<int16_t>(sinf(phase) * 8000); // moderate volume
        samples[i * 2] = sample;     // left
        samples[i * 2 + 1] = sample; // right
        phase += kPhaseIncrement;
        if (phase > 2.0f * PI) phase -= 2.0f * PI;
    }

    return frameCount * 4;
}

} // namespace

void BluetoothSource::begin(const char *targetDeviceName) {
    if (running) return;

    // Internal-heap guard FIRST -- a real crash in the field
    // (semphr_create_wrapper assert, then separately a WiFi esp_timer_create
    // abort) traced back to low internal DRAM headroom right after the
    // boot-time library scan, not the WiFi/BT timing race RadioLock alone
    // was built to prevent. See RadioLock.h/CLAUDE.md.
    if (!radioHeapOk("BluetoothSource")) return;

    // Holds the radio lock only around the actual radio-touching call
    // below (start()), NOT for the whole connected session -- see
    // RadioLock.h's updated comment. Previously held until end(), which
    // meant TimeSync could never sync at all for as long as BT stayed on
    // (commonly hours), not just during the brief moment BT is actually
    // initiating its connection. If TimeSync's WiFi is mid-cycle right
    // now, this just doesn't start; try again in a moment (a TimeSync
    // cycle is short-lived).
    if (!RadioLock::tryAcquire()) {
        Serial.println(F("[bt] can't start yet -- WiFi time sync is active, try again shortly"));
        return;
    }

    Serial.printf("[bt] Starting Bluetooth A2DP source... (free heap: %u bytes)\n", ESP.getFreeHeap());
    a2dpSource.set_data_callback(provideTestTone);
    a2dpSource.start(targetDeviceName);
    RadioLock::release(); // see above -- don't hold this past the actual start() call
    running = true;
    Serial.printf("[bt] A2DP source scanning for \"%s\" -- put it in "
                  "pairing/discoverable mode; you should hear a 440Hz tone "
                  "once connected.\n",
                  targetDeviceName);
}

void BluetoothSource::end() {
    if (!running) return;
    a2dpSource.end();
    running = false;
    // No RadioLock::release() here -- begin() no longer holds the lock
    // this far (see its comment), so releasing here would incorrectly
    // clear RadioLock::busy if TimeSync happens to hold it at this exact
    // moment, letting BluetoothSource's own next begin() (or anything
    // else) acquire it while TimeSync still thinks it's mid-cycle.
    Serial.println(F("[bt] A2DP source stopped"));
}

bool BluetoothSource::isConnected() { return running && a2dpSource.is_connected(); }
bool BluetoothSource::isRunning() { return running; }
