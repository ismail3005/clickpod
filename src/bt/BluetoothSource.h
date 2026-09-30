#pragma once

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
// Bluetooth settings works. Put your headphones/speaker's exact BT name
// here and make sure they're in pairing/discoverable mode when turning
// Bluetooth on from the UI.
constexpr const char *kTargetDeviceName = "ULT WEAR";

// targetDeviceName is the name of the SINK device to scan for and connect
// to (your headphones/speaker's actual BT name) -- source mode actively
// seeks out a known target, it doesn't advertise itself to be paired with.
void begin(const char *targetDeviceName);

// Stops the A2DP source and disconnects. Safe to call even if never
// begun/already stopped.
void end();

bool isConnected();
bool isRunning(); // true once begin() has been called and not yet end()'d

} // namespace BluetoothSource
