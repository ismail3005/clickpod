#pragma once

#include <atomic>
#include <esp_heap_caps.h>

#include <Arduino.h>

// Minimum free INTERNAL (non-PSRAM) heap required before starting WiFi or
// classic Bluetooth. Both subsystems' controller/driver buffers must come
// from internal, DMA-capable RAM -- PSRAM doesn't count, and ESP.getFreeHeap()
// alone is misleading here because it can report plenty of headroom that's
// actually all PSRAM while internal RAM is the thing that's actually
// scarce. Hit in the field twice now: BT failed a semaphore/queue alloc
// (`semphr_create_wrapper`) right after WiFi itself failed an rx-buffer
// alloc, and separately WiFi's own esp_timer_create aborted the whole
// device with ESP_ERR_NO_MEM -- both shortly after the ~425-track library
// scan, which is the obvious concurrent consumer of internal heap at that
// point in boot. This threshold is a conservative placeholder, not a
// number backed by ESP-IDF documentation of exact WiFi/BT minimums (that
// figure isn't reliably published and depends on config) -- tune it down
// if the real logged numbers below it turn out comfortably safe, or up if
// it still isn't enough headroom.
constexpr size_t kMinInternalHeapForRadio = 60 * 1024;

// Logs the current internal-heap headroom and returns whether it's safe to
// start WiFi or BT right now. Call this BEFORE touching either subsystem --
// unlike a crash inside their own init code (which aborts the whole
// device, unrecoverable from app code), this lets the caller skip/defer
// instead, which is the only way to actually avoid the abort().
inline bool radioHeapOk(const char *who) {
    size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[radio] %s: free internal heap %u bytes (need >= %u)\n",
                  who, (unsigned)freeInternal, (unsigned)kMinInternalHeapForRadio);
    if (freeInternal < kMinInternalHeapForRadio) {
        Serial.printf("[radio] %s: skipping -- not enough internal heap headroom, "
                      "would likely crash the device instead of just failing to start\n",
                      who);
        return false;
    }
    return true;
}

// The ESP32 has ONE radio shared between WiFi (TimeSync) and classic
// Bluetooth (BluetoothSource) -- letting both touch it at once (e.g. a
// TimeSync scan/connect cycle running while BT is starting up) is a real,
// documented class of crash on this chip, not a hypothetical. Strong
// suspect for a crash seen right after TimeSync's WiFi usage was added:
// `[bt] Starting Bluetooth A2DP source...` followed immediately by
// `assert failed: hash_map_set hash_map.c:129` and a reboot, twice in a
// row in the field. Not confirmed via a reproduction (no hardware access
// here), but the timing correlation plus ESP32's known WiFi+BT
// coexistence issues make it the leading explanation -- if this doesn't
// fully fix it, that's the next thing to revisit.
//
// Whichever subsystem is using the radio holds this for its WHOLE active
// duration (BluetoothSource from begin() to end(), not just start/stop;
// TimeSync for the length of one scan+connect+NTP cycle) -- the other
// simply skips/defers its own radio use rather than risk an overlap.
// Simple by design: this is about avoiding a crash, not maximizing radio
// uptime for either side.
namespace RadioLock {

inline std::atomic<bool> busy{false};

inline bool tryAcquire() {
    bool expected = false;
    return busy.compare_exchange_strong(expected, true);
}

inline void release() { busy = false; }
inline bool isBusy() { return busy; }

// RAII guard for a short-lived acquisition (TimeSync's one-shot sync
// cycle) -- releases automatically on any return path. BluetoothSource
// holds the lock directly (not via this guard) since its "active
// duration" spans from begin() to a separate end() call, not one
// function's lifetime.
struct ScopedLock {
    bool acquired;
    ScopedLock() : acquired(tryAcquire()) {}
    ~ScopedLock() { if (acquired) release(); }
};

} // namespace RadioLock
