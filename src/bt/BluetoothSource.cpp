#include "BluetoothSource.h"
#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <math.h>
#include <cstring>

#include "../net/RadioLock.h"

namespace {

BluetoothA2DPSource a2dpSource;
bool running = false;
String currentTarget; // name last passed to begin(), "" if never begun

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

// --- Discovery stash ---
// Written only from ssidCallback (the BT stack's own task context), read
// only from the main loop via the public discoveredCount()/discoveredName()
// getters below -- single producer, single consumer, append-only (never
// removed/modified once written), same risk profile this codebase already
// accepts for AnoInput's encoder ISR accumulator. Fixed-size, no heap
// allocation from the callback: a BT-stack callback is not a context to
// risk a malloc/String allocation failure or fragmentation in.
constexpr int kMaxDiscovered = 24;
char discoveredNames[kMaxDiscovered][32];
volatile int discoveredCountVal = 0;

bool ssidCallback(const char *ssid, esp_bd_addr_t /*address*/, int /*rssi*/) {
    if (!ssid || ssid[0] == '\0') return false;
    int count = discoveredCountVal; // snapshot -- only this task ever increments it
    for (int i = 0; i < count; i++) {
        if (strncmp(discoveredNames[i], ssid, sizeof(discoveredNames[i]) - 1) == 0) {
            return false; // already stashed, keep scanning
        }
    }
    if (count < kMaxDiscovered) {
        strncpy(discoveredNames[count], ssid, sizeof(discoveredNames[count]) - 1);
        discoveredNames[count][sizeof(discoveredNames[count]) - 1] = '\0';
        discoveredCountVal = count + 1; // publish AFTER the name is fully written
    }
    return false; // never auto-select -- this is pure listing, picking happens via connectToDiscovered()
}

} // namespace

void BluetoothSource::begin(const char *targetDeviceName, bool allowAutoReconnect) {
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
    a2dpSource.set_ssid_callback(nullptr); // ensure a prior discovery scan's callback isn't still armed
    a2dpSource.set_auto_reconnect(allowAutoReconnect); // see BluetoothSource.h's comment on this parameter
    a2dpSource.start(targetDeviceName);
    RadioLock::release(); // see above -- don't hold this past the actual start() call
    running = true;
    currentTarget = targetDeviceName;
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
const char *BluetoothSource::currentTargetName() { return currentTarget.c_str(); }

void BluetoothSource::startDiscovery() {
    if (running) return; // don't fight an already-connecting/connected session
    if (!radioHeapOk("BluetoothSource-discovery")) return;
    if (!RadioLock::tryAcquire()) {
        Serial.println(F("[bt] can't scan yet -- WiFi time sync is active, try again shortly"));
        return;
    }
    discoveredCountVal = 0;
    a2dpSource.set_data_callback(provideTestTone);
    a2dpSource.set_ssid_callback(ssidCallback);
    a2dpSource.start(); // no name -- pure discovery, see BluetoothSource.h
    RadioLock::release(); // same brief-hold pattern as begin() -- discovery itself runs async on the BT task
    Serial.println(F("[bt] scanning for nearby devices..."));
}

void BluetoothSource::cancelDiscovery() {
    a2dpSource.cancel_discovery();
}

bool BluetoothSource::isDiscoveryActive() { return a2dpSource.is_discovery_active(); }

int BluetoothSource::discoveredCount() { return discoveredCountVal; }

const char *BluetoothSource::discoveredName(int index) {
    if (index < 0 || index >= discoveredCountVal) return "";
    return discoveredNames[index];
}

void BluetoothSource::connectToDiscovered(const char *name) {
    a2dpSource.cancel_discovery();
    a2dpSource.set_ssid_callback(nullptr);
    running = false; // startDiscovery() never set this true -- begin() below starts the real connection fresh
    // allowAutoReconnect=false: the user is explicitly picking a device by
    // name from the real scan results -- if a DIFFERENT device was
    // already bonded before, auto-reconnect would silently ignore this
    // name and reconnect to that old one instead (confirmed from the
    // library source: the auto-reconnect branch bypasses the name/
    // discovery path entirely whenever a stored last-connection exists).
    // Forcing a real name-based scan here is what lets the library's own
    // successful-connection handler (filter_inquiry_scan_result()) update
    // its stored "last connection" to THIS device, so every subsequent
    // normal begin() (status row, boot auto-resume) correctly auto-
    // reconnects to the newly-picked device from then on.
    begin(name, /*allowAutoReconnect=*/false);
}
