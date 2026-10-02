#include "BluetoothSource.h"
#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <math.h>
#include <cstring>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "../net/RadioLock.h"

namespace {

// --- Bonded-address shadow, working around a real ESP32-A2DP library bug ---
//
// Confirmed by reading the real library source (BluetoothA2DPCommon.cpp):
// end() unconditionally calls clean_last_connection(), which stores an
// ALL-ZERO address into both its in-RAM last_connection member and its own
// NVS blob (namespace "connected_bda", key "src_bda" for Source mode) --
// every single time Bluetooth is stopped, even if it had just bonded
// successfully moments earlier. The next start()'s internal
// get_last_connection() then finds a validly-stored-but-zero address (NOT
// "no address" -- nvs_get_blob succeeds, it just reads back zeros), so it
// silently falls through to a fresh discovery scan instead of reconnecting
// by address -- this is the real, confirmed reason a device that paired
// fine still needs pairing mode again on every subsequent "Bluetooth On".
// (set_last_connection()/get_last_connection()/has_last_connection()/
// last_bda_nvs_name() are all `protected`, can't be called from here --
// but get_last_peer_address() is public and readable, and the library's
// NVS namespace/key names are stable implementation details read directly
// from source, not guessed.)
//
// Fix: snapshot the real address ourselves (our own NVS slot, so it
// survives a reboot too) right before every end(), then re-seed the
// LIBRARY's own NVS blob with it right before the next begin() that wants
// auto-reconnect -- so by the time start()'s internal get_last_connection()
// runs, it finds a real address again instead of the zero one end() just
// wrote.
constexpr char kShadowNvsNamespace[] = "cpod_bt";
constexpr char kShadowNvsKey[] = "last_bda";
constexpr char kLibraryNvsNamespace[] = "connected_bda";
constexpr char kLibraryNvsKey[] = "src_bda"; // BluetoothA2DPSource::last_bda_nvs_name()

uint8_t shadowBda[6] = {0};
bool shadowLoaded = false; // lazy-loaded from our own NVS once per boot

bool isZeroBda(const uint8_t *bda) {
    for (int i = 0; i < 6; i++) {
        if (bda[i] != 0) return false;
    }
    return true;
}

void loadShadowOnce() {
    if (shadowLoaded) return;
    shadowLoaded = true;
    nvs_handle_t h;
    if (nvs_open(kShadowNvsNamespace, NVS_READONLY, &h) != ESP_OK) return; // never saved yet
    size_t len = sizeof(shadowBda);
    nvs_get_blob(h, kShadowNvsKey, shadowBda, &len);
    nvs_close(h);
}

void saveShadow(const uint8_t *bda) {
    memcpy(shadowBda, bda, sizeof(shadowBda));
    shadowLoaded = true;
    nvs_handle_t h;
    if (nvs_open(kShadowNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, kShadowNvsKey, shadowBda, sizeof(shadowBda));
    nvs_commit(h);
    nvs_close(h);
}

// Writes directly into the LIBRARY's own NVS blob -- the same one
// clean_last_connection() just zeroed -- so its next get_last_connection()
// (called from inside start()) finds our restored real address instead.
void reseedLibraryNvs(const uint8_t *bda) {
    nvs_handle_t h;
    if (nvs_open(kLibraryNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, kLibraryNvsKey, bda, 6);
    nvs_commit(h);
    nvs_close(h);
}

BluetoothA2DPSource a2dpSource;
bool running = false;
String currentTarget; // name last passed to begin(), "" if never begun

// See tick()/BluetoothSource.h's comment -- 0 means "not currently
// searching" (either never begun, or currently connected). Set to
// millis() the moment a disconnected/searching state is first observed,
// reset to 0 the moment a connection lands -- so the give-up window
// always measures from the start of the CURRENT search, not from the
// original begin() call hours earlier if it had connected and later
// dropped.
uint32_t searchingSinceMs = 0;
constexpr uint32_t kGiveUpMs = 60000; // 1 minute of fruitless searching

// Real audio ring buffer -- fed by AudioBridge.cpp's audio_process_i2s()
// weak-symbol override (ESP32-audioI2S's own documented hook, literally
// commented "record audiodata or send via BT" in its header) with every
// decoded PCM buffer, already 44.1kHz 16-bit stereo interleaved. Producer
// (feedPcm, called from the main/audio task) and consumer (providePcm,
// called from the BT stack's own task) run on different tasks, so this
// needs real synchronization -- a plain FreeRTOS mutex, not an ISR
// context on either side.
constexpr size_t kPcmRingSize = 16384; // ~93ms of headroom at 44.1kHz/16-bit/stereo
uint8_t *pcmRing = nullptr; // ps_malloc'd lazily, PSRAM (this project's established push)
SemaphoreHandle_t pcmMutex = nullptr;
size_t pcmHead = 0, pcmTail = 0, pcmCount = 0;

void ensurePcmRing() {
    if (pcmRing) return;
    pcmRing = (uint8_t *)ps_malloc(kPcmRingSize);
    pcmMutex = xSemaphoreCreateMutex();
}

// Pull callback ESP32-A2DP's source calls on its own task whenever it
// needs more bytes. Drains the ring buffer; underruns (not enough
// decoded audio buffered yet) fill with silence rather than garbage --
// a brief silent gap reads far better than noise.
int32_t providePcm(uint8_t *data, int32_t byteCount) {
    ensurePcmRing();
    if (!pcmRing || !pcmMutex) {
        memset(data, 0, byteCount);
        return byteCount;
    }
    xSemaphoreTake(pcmMutex, portMAX_DELAY);
    int32_t n = (int32_t)min((size_t)byteCount, pcmCount);
    for (int32_t i = 0; i < n; i++) {
        data[i] = pcmRing[pcmTail];
        pcmTail = (pcmTail + 1) % kPcmRingSize;
    }
    pcmCount -= n;
    xSemaphoreGive(pcmMutex);
    if (n < byteCount) memset(data + n, 0, byteCount - n);
    return byteCount;
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
    searchingSinceMs = 0; // fresh search window -- see tick()'s comment

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

    // Re-seed the library's own "last connection" NVS blob with our shadow
    // copy BEFORE start() -- see the shadow-save block above. end() always
    // zeroes the library's real copy, so without this every begin() after
    // the first would find nothing to reconnect to and fall back to a
    // fresh discovery scan (pairing mode required) even for an
    // already-bonded device. Skipped for an explicit device pick
    // (allowAutoReconnect=false) -- that path wants a real scan by name.
    if (allowAutoReconnect) {
        loadShadowOnce();
        if (!isZeroBda(shadowBda)) {
            reseedLibraryNvs(shadowBda);
        }
    }

    Serial.printf("[bt] Starting Bluetooth A2DP source... (free heap: %u bytes)\n", ESP.getFreeHeap());
    a2dpSource.set_data_callback(providePcm);
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
    // Snapshot the real bonded address BEFORE calling the library's end(),
    // which (confirmed from source, see the shadow-save block above)
    // unconditionally wipes its own copy to all-zero -- this is the only
    // chance to save it before that happens.
    const uint8_t *liveBda = reinterpret_cast<const uint8_t *>(a2dpSource.get_last_peer_address());
    if (liveBda && !isZeroBda(liveBda)) {
        saveShadow(liveBda);
    }
    // Diagnostic logging, mirroring begin()'s -- a real-world report of
    // the device crash-rebooting while turning Bluetooth off during an
    // active reconnect loop has no serial log to point at a cause yet
    // (happened running on battery, untethered). end()'s teardown
    // (disconnect(), AVRC deinit, its own NVS writes) does real heap
    // allocation internally, same class of risk as begin()'s own guarded
    // start() call -- next crash with a serial monitor attached, this is
    // the number to check first instead of guessing blind again.
    Serial.printf("[bt] Stopping Bluetooth A2DP source... (free internal heap: %u bytes)\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
    a2dpSource.set_data_callback(providePcm);
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

void BluetoothSource::feedPcm(const uint8_t *data, size_t len) {
    ensurePcmRing();
    if (!pcmRing || !pcmMutex) return;
    xSemaphoreTake(pcmMutex, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        if (pcmCount >= kPcmRingSize) { // overflow: drop oldest, keep latest audio
            pcmTail = (pcmTail + 1) % kPcmRingSize;
            pcmCount--;
        }
        pcmRing[pcmHead] = data[i];
        pcmHead = (pcmHead + 1) % kPcmRingSize;
        pcmCount++;
    }
    xSemaphoreGive(pcmMutex);
}

void BluetoothSource::tick() {
    if (!running) return;
    if (a2dpSource.is_connected()) {
        searchingSinceMs = 0; // connected -- not searching, reset so a LATER drop gets its own fresh window
        return;
    }
    if (searchingSinceMs == 0) {
        searchingSinceMs = millis(); // first tick observed as disconnected/searching
        return;
    }
    if (millis() - searchingSinceMs >= kGiveUpMs) {
        Serial.println(F("[bt] giving up after a minute of searching with no connection -- turning off "
                          "(use the Bluetooth menu to try again)"));
        end();
    }
}
