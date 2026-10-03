#include "BluetoothSource.h"
#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <math.h>
#include <cstring>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

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

// See tick()/BluetoothSource.h's comment. searchingSinceMs: 0 means "not
// currently searching for the FIRST connection of this begin() session"
// -- set to millis() the moment a not-yet-ever-connected search starts,
// reset to 0 once it lands. kGiveUpMs only bounds THIS first-connection
// search (a normal "Bluetooth On" with the device briefly out of range
// should get a real chance) -- a disconnect AFTER a real connection is
// handled completely differently, immediately, see tick().
uint32_t searchingSinceMs = 0;
constexpr uint32_t kGiveUpMs = 60000; // 1 minute to land the FIRST connection
bool hasEverConnectedThisSession = false;

// Real audio ring buffer -- fed by AudioBridge.cpp's audio_process_i2s()
// weak-symbol override (ESP32-audioI2S's own documented hook, literally
// commented "record audiodata or send via BT" in its header) with every
// decoded PCM buffer, already 44.1kHz 16-bit stereo interleaved. Producer
// (feedPcm, called from the main/audio task) and consumer (providePcm,
// called from the BT stack's own task) run on different tasks, so this
// needs real synchronization -- a plain FreeRTOS mutex, not an ISR
// context on either side.
//
// Real hardware report: audio over BT was "laggy and noisy and glitchy
// and overall unusable." Root cause wasn't the buffer being too small
// alone -- it's that feedPcm() (below) used to DROP the oldest buffered
// audio on overflow instead of pushing back on the producer. Skipping
// the I2S write (continueI2S=false) removes the one thing that was
// naturally pacing the decode loop to real-time -- a blocking I2S write
// takes roughly as long as the audio it writes actually takes to play.
// Without that, nothing stopped the decoder from producing chunks
// faster than A2DP drains them, and "drop oldest" silently threw away
// chunks of audio MID-STREAM -- which is exactly what a glitchy,
// discontinuous-sounding stream is. 16KB (~93ms) was also just thin
// margin for real BT link jitter on top of that. Both fixed below:
// feedPcm() now blocks (bounded) for room instead of dropping, which
// re-creates the pacing a real I2S write would have provided, and the
// buffer is bumped to give real jitter headroom -- PSRAM is abundant
// (4MB, see this project's established PSRAM push) and a player doesn't
// care about a few hundred ms of extra latency the way a call would.
constexpr size_t kPcmRingSize = 65536; // ~372ms of headroom at 44.1kHz/16-bit/stereo
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

// Real AVRCP passthrough key codes (ESP_AVRC_PT_CMD_*) -- used directly
// by the library's own BluetoothA2DPSink.cpp (execute_avrc_command()),
// not guessed: PLAY=0x44, PAUSE=0x46, FORWARD(next)=0x4B,
// BACKWARD(previous)=0x4C, standard AVRCP 1.x operation IDs.
volatile BluetoothSource::TransportCmd pendingTransportCmd = BluetoothSource::TransportCmd::None;

void passthruCallback(uint8_t keyCode, bool isReleased) {
    if (isReleased) return; // act once, on press -- not on both press and release
    switch (keyCode) {
        case ESP_AVRC_PT_CMD_PLAY: pendingTransportCmd = BluetoothSource::TransportCmd::Play; break;
        case ESP_AVRC_PT_CMD_PAUSE: pendingTransportCmd = BluetoothSource::TransportCmd::Pause; break;
        case ESP_AVRC_PT_CMD_FORWARD: pendingTransportCmd = BluetoothSource::TransportCmd::Next; break;
        case ESP_AVRC_PT_CMD_BACKWARD: pendingTransportCmd = BluetoothSource::TransportCmd::Previous; break;
        default: break;
    }
}

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
    hasEverConnectedThisSession = false;

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
    a2dpSource.set_avrc_passthru_command_callback(passthruCallback);
    // max_retries=0 is the real crash fix, not just the "stop hammering
    // forever" UX one -- see the real crash log this was traced to:
    // end() was called while the library's own heartbeat-driven
    // reconnect-by-address (handle_reconnect_logic(), retries defaulted
    // to ~1000) had an esp_a2d_connect() actively in flight. Confirmed
    // from the real library source: BluetoothA2DPSource::end() only
    // waits out an in-flight DISCOVERY scan (`while(discovery_active)
    // delay_ms(100)`) before tearing down -- there is NO equivalent
    // guard for an in-flight CONNECT attempt, so end() and the BT
    // stack's own event task (bt_app_task, running handle_reconnect_
    // logic()/connect_to() concurrently) raced on the same AVRC/GAP/A2D
    // state with nothing serializing them -- a real Guru Meditation
    // LoadProhibited crash (EXCVADDR near-null), not heap exhaustion.
    // With max_retries=0, a disconnect's very first heartbeat check
    // takes handle_reconnect_logic()'s OTHER branch (a one-shot
    // discovery scan, not connect_to()) -- which end() DOES safely wait
    // out. This doesn't just reduce the crash's likelihood, it removes
    // the specific unsafe state (an in-flight connect_to()) end() can
    // ever be called into.
    a2dpSource.set_auto_reconnect(allowAutoReconnect, 0);
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
    // Real fix for the reported multi-second freeze on "Turn Bluetooth
    // Off" (the busy-message was only ever a cosmetic cover for this,
    // not a fix) -- confirmed from the real library source:
    // BluetoothA2DPSource::end() does `while(discovery_active)
    // delay_ms(100);` before tearing anything down, and critically,
    // that flag only clears when the CURRENT inquiry window ends
    // naturally (ESP_BT_GAP_DISC_STATE_CHANGED_EVT ->
    // ESP_BT_GAP_DISCOVERY_STOPPED) -- end() sets `is_end=true` so a
    // NEW scan won't start after that, but does nothing to cut the
    // one already running short. Each scan is started with
    // `esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10,
    // 0)` -- a ~10-unit (~12.8s) inquiry window -- so calling end()
    // mid-scan means waiting out however much of that window is left,
    // exactly matching the observed ~9s stall. Cancelling the scan
    // FIRST (a real, already-used-elsewhere library call,
    // esp_bt_gap_cancel_discovery() under the hood) makes discovery_active
    // clear almost immediately instead of waiting for the window to
    // expire on its own. Guarded by is_discovery_active() so this is a
    // no-op (not an extra GAP call) on the much more common case where
    // discovery isn't running at all.
    if (a2dpSource.is_discovery_active()) {
        a2dpSource.cancel_discovery();
    }
    a2dpSource.end();
    running = false;
    // No RadioLock::release() here -- begin() no longer holds the lock
    // this far (see its comment), so releasing here would incorrectly
    // clear RadioLock::busy if TimeSync happens to hold it at this exact
    // moment, letting BluetoothSource's own next begin() (or anything
    // else) acquire it while TimeSync still thinks it's mid-cycle.
    Serial.println(F("[bt] A2DP source stopped"));
}

void BluetoothSource::setVolume(uint8_t volume0to127) {
    if (!running) return;
    a2dpSource.set_volume(volume0to127);
}

uint8_t BluetoothSource::getVolume() { return (uint8_t)a2dpSource.get_volume(); }

BluetoothSource::TransportCmd BluetoothSource::drainTransportCommand() {
    TransportCmd cmd = pendingTransportCmd;
    pendingTransportCmd = TransportCmd::None;
    return cmd;
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
    // Real backpressure instead of drop-oldest-on-overflow -- see the
    // big comment on kPcmRingSize above for why dropping was the actual
    // cause of the glitchy/noisy audio. When the buffer's full, wait in
    // short bursts for the consumer (providePcm(), the BT stack's own
    // task) to drain some before writing more -- this is what re-creates
    // the real-time pacing a blocking I2S write used to provide. Bounded
    // total wait (kMaxWaitMs) so a stalled/disconnected consumer can't
    // hang the main loop (audio.loop() calls this synchronously) forever
    // -- past that, the rest of this one chunk is dropped (logged once),
    // which is a world apart from silently dropping continuously.
    constexpr uint32_t kMaxWaitMs = 250;
    uint32_t waitStart = millis();
    size_t written = 0;
    while (written < len) {
        xSemaphoreTake(pcmMutex, portMAX_DELAY);
        size_t space = kPcmRingSize - pcmCount;
        size_t chunk = min(space, len - written);
        for (size_t i = 0; i < chunk; i++) {
            pcmRing[pcmHead] = data[written + i];
            pcmHead = (pcmHead + 1) % kPcmRingSize;
        }
        pcmCount += chunk;
        written += chunk;
        xSemaphoreGive(pcmMutex);
        if (written >= len) break;
        if (millis() - waitStart >= kMaxWaitMs) {
            Serial.println(F("[bt] PCM ring buffer stayed full too long -- dropping the rest of this chunk"));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2)); // brief yield, let the consumer task actually run and drain
    }
}

void BluetoothSource::tick() {
    if (!running) return;
    if (a2dpSource.is_connected()) {
        hasEverConnectedThisSession = true;
        searchingSinceMs = 0;
        return;
    }
    // Real disconnect AFTER a real connection (headphones powered off,
    // walked out of range, etc.) -- user's explicit ask: "once a device
    // is disconnected, turn off and stop looking for that device."
    // Turning off HERE, immediately, rather than waiting for the
    // kGiveUpMs window below, is also the safest timing: max_retries=0
    // (see begin()) means the library's own heartbeat won't have
    // started a risky in-flight connect_to() yet on this very first
    // post-disconnect tick, so end() is called into a quiescent state,
    // not racing an active reconnect attempt the way the real crash did.
    if (hasEverConnectedThisSession) {
        Serial.println(F("[bt] device disconnected -- turning Bluetooth off (not auto-searching for it again)"));
        end();
        return;
    }
    // Below here: still trying to land the FIRST connection of this
    // begin() session (e.g. device was out of range/off when "Bluetooth
    // On" was pressed) -- give it a real, bounded chance rather than
    // ending instantly, since that's a normal, expected case, not a
    // disconnect.
    if (searchingSinceMs == 0) {
        searchingSinceMs = millis(); // first tick observed as still-searching
        return;
    }
    if (millis() - searchingSinceMs >= kGiveUpMs) {
        Serial.println(F("[bt] giving up after a minute without finding the device -- turning off "
                          "(use the Bluetooth menu to try again)"));
        end();
    }
}
