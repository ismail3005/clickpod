#pragma once

// Bring-up step 5: proves the ESP32's classic Bluetooth radio + A2DP
// source path works on real hardware in isolation -- scans for and
// auto-connects to a target BT sink device (headphones/speaker) and
// streams a simple sine-wave test tone. Not wired into the actual FLAC
// player yet; per docs/SPEC.md section 7, wired (I2S) and Bluetooth are
// two separate, mutually-exclusive output paths that the user manually
// switches between via the BT menu, which doesn't exist yet (that's real
// UI/UX work, section 6).
namespace BluetoothSource {

// targetDeviceName is the name of the SINK device to scan for and connect
// to (your headphones/speaker's actual BT name) -- source mode actively
// seeks out a known target, it doesn't advertise itself to be paired with.
void begin(const char *targetDeviceName);

} // namespace BluetoothSource
