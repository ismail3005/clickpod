#include "BluetoothSource.h"
#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <math.h>

namespace {

BluetoothA2DPSource a2dpSource;

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
    Serial.println(F("[bringup] Starting Bluetooth A2DP source..."));
    a2dpSource.set_data_callback(provideTestTone);
    a2dpSource.start(targetDeviceName);
    Serial.printf("[bringup] A2DP source scanning for \"%s\" -- put it in "
                  "pairing/discoverable mode; you should hear a 440Hz tone "
                  "once connected.\n",
                  targetDeviceName);
}
