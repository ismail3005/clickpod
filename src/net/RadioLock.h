#pragma once

#include <atomic>

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
