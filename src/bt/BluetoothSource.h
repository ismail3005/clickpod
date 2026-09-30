#pragma once

// Bring-up step 5: proves the ESP32's classic Bluetooth radio + A2DP
// source path works on real hardware in isolation -- pairs to a nearby
// BT speaker/headphones and streams a simple sine-wave test tone. Not
// wired into the actual FLAC player yet; per docs/SPEC.md section 7,
// wired (I2S) and Bluetooth are two separate, mutually-exclusive output
// paths that the user manually switches between via the BT menu, which
// doesn't exist yet (that's real UI/UX work, section 6).
namespace BluetoothSource {

// deviceName is what shows up when pairing (e.g. "clickpod").
void begin(const char *deviceName);

} // namespace BluetoothSource
