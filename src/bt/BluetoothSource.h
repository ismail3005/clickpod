#pragma once

#include <Arduino.h>

// Bring-up step 5 -> now wired into the real UI's Bluetooth screen
// (src/ui/MenuEngine.cpp's enterBluetooth()/exitBluetooth()) instead of
// being an isolated test-only path. Still streams a 440Hz test tone, NOT
// real FLAC playback -- routing ESP32-audioI2S's decoded PCM into the A2DP
// source's callback instead of out to the I2S DAC is a separate, bigger
// piece of work (a real dual-output audio pipeline), not done here. This
// only makes the UI's on/off + connection status REAL instead of a mock
// device list -- see docs/SPEC.md section 7 for the wired/BT output
// split this still respects.
namespace BluetoothSource {

// In A2DP SOURCE mode this is the name of the target SINK device to scan
// for and auto-connect to (e.g. your headphones/speaker) -- NOT the
// ESP32's own advertised name. Source actively seeks out a known sink by
// name, the reverse of how a peripheral you'd pair to from a phone's
// Bluetooth settings works. Used as the fallback target when no device
// has ever been picked from the real device-picker screen (see below) --
// once the user picks one, Persist remembers it and this default is no
// longer used on that board.
constexpr const char *kTargetDeviceName = "ULT WEAR";

// targetDeviceName is the name of the SINK device to scan for and connect
// to (your headphones/speaker's actual BT name) -- source mode actively
// seeks out a known target, it doesn't advertise itself to be paired with.
//
// allowAutoReconnect (default true): ESP32-A2DP has real NVS-backed
// bonding already built in (set_last_connection()/get_last_connection(),
// confirmed by reading the real library source) -- with it enabled, a
// connect to a device that's already bonded skips the name-based
// discovery scan entirely and reconnects straight to the stored address,
// which is also why it doesn't need the device back in pairing/
// discoverable mode (that's only required to be FOUND during a scan, not
// to accept a direct reconnect from an already-bonded peer). Previously
// never enabled, which is why every reconnect -- even to a device that
// had connected successfully several times before -- still required
// pairing mode again. Pass false specifically when the user is
// explicitly picking a device from the real picker screen (see
// connectToDiscovered() below): that path needs a genuine scan by name,
// not a silent reconnect to whatever was bonded before, since the user
// may be choosing a DIFFERENT device than the last one.
void begin(const char *targetDeviceName, bool allowAutoReconnect = true);

// Stops the A2DP source and disconnects. Safe to call even if never
// begun/already stopped.
void end();

bool isConnected();
bool isRunning(); // true once begin() has been called and not yet end()'d

// The name last passed to begin() (whatever's currently being sought/
// connected), or "" if never begun this session. Used by main.cpp's
// syncBluetoothToUi() to show the real connected name instead of always
// assuming kTargetDeviceName, now that the target can be a user-picked
// device.
const char *currentTargetName();

// --- Device discovery (picker screen) ---
//
// ESP32-A2DP's source mode genuinely supports discovery: start() with no
// name begins a scan instead of connecting to a fixed target, and
// set_ssid_callback() fires once per compatible device found during that
// scan, on the BT stack's OWN task context -- not the main loop, so it
// can't touch UI/MenuEngine state directly (same constraint AnoInput's
// encoder ISR has). startDiscovery() installs a callback that just
// stashes each newly-seen device's name into a small fixed-size array
// (never allocates from that callback context) for the main loop to
// drain via discoveredCount()/discoveredName() -- MenuEngine builds the
// picker screen's rows from those, polling for new arrivals while the
// screen is open (see UI.cpp).
void startDiscovery();
void cancelDiscovery();
bool isDiscoveryActive();
int discoveredCount();
const char *discoveredName(int index);

// Stops discovery and connects to exactly this (already-discovered)
// device name, reusing begin()'s existing heap-guard/RadioLock path --
// equivalent to begin(name), just named for the picker call site's
// clarity.
void connectToDiscovered(const char *name);

// Real audio output -- replaces the old 440Hz test tone. Called from
// AudioBridge.cpp's audio_process_i2s() (a weak-symbol hook
// ESP32-audioI2S itself exposes, literally commented "record audiodata
// or send via BT" in its own header) with every decoded PCM buffer,
// already 44.1kHz 16-bit stereo interleaved -- exactly what A2DP wants,
// no conversion needed. Buffers into a small ring the A2DP data callback
// (running on the BT stack's own task, not the caller's) drains from;
// thread-safe via an internal mutex since producer and consumer run on
// different tasks. Safe to call even when not connected (no-ops).
void feedPcm(const uint8_t *data, size_t len);

} // namespace BluetoothSource
