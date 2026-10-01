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
// documented class of crash on this chip, not a hypothetical. Original
// suspect for a crash seen right after TimeSync's WiFi usage was added:
// `[bt] Starting Bluetooth A2DP source...` followed immediately by
// `assert failed: hash_map_set hash_map.c:129` and a reboot. That
// diagnosis turned out to be WRONG (or at least incomplete) -- see
// CLAUDE.md's seventh/eighth hardware bug writeup -- the real cause was
// internal-heap exhaustion right after the boot-time library scan, now
// independently guarded by radioHeapOk() above, on both subsystems.
//
// IMPORTANT, found later (CLAUDE.md's fifteenth bug): this lock used to be
// held by BluetoothSource for its WHOLE connected session (begin() to
// end()), not just the brief moment it actually touches the radio to
// start. Since BT commonly stays on for hours, that meant TimeSync could
// NEVER acquire the lock -- never scan, never sync -- for as long as BT was
// on, not a brief collision window. Narrowed to cover only the actual
// radio-touching call (a2dpSource.start()) -- BluetoothSource now releases
// the lock right after that returns, same brief-hold pattern TimeSync
// itself already used (one scan+connect+NTP cycle, not "for as long as
// WiFi might ever be wanted"). This still prevents a literal same-instant
// collision (TimeSync scanning the exact moment BT is initiating a
// connection) without permanently starving TimeSync out whenever BT
// happens to be on, which is what actually happened before this fix.
// Honest residual gap: ESP32-A2DP's start() is asynchronous -- it kicks
// off scanning/pairing on its own BT task and returns quickly, so the
// lock window here doesn't cover BT's whole multi-second connection
// handshake, only the initial call. Given the original "simultaneous
// radio use crashes" theory was never actually confirmed (see above --
// it was heap exhaustion), this is judged an acceptable tradeoff rather
// than a proven-safe one; if a real WiFi/BT collision crash does surface
// again, this is the first place to revisit, probably by having
// BluetoothSource poll for a stable is_connected() before releasing.
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
