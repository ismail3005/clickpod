#pragma once

#include <Arduino.h>

// Grabs wall-clock time "for free" -- no configured WiFi credentials, no
// RTC chip (none in the BOM). Scans for an open (no-password) WiFi network
// nearby, joins it briefly, fetches NTP time, then disconnects and radios
// off, keeping time locally via millis() from then on. Re-attempts
// periodically to correct for clock drift and to cover the case where no
// open network was found the first time.
//
// Runs entirely on a background FreeRTOS task (core 0), so it never blocks
// boot or normal use. Unlike backgrounding the SD library scan (deliberately
// NOT done -- see CLAUDE.md), this carries no cross-peripheral risk: WiFi
// doesn't touch the SPI bus TFT/SD share. It DOES share the ESP32's single
// radio with classic Bluetooth, though -- a scan+sync attempt may cause
// brief BT audio glitches if BT is actively streaming at that moment; not
// worked around given BT playback is currently just a test tone anyway
// (see BluetoothSource.h), worth revisiting once BT streams real audio.
//
// Depends only on core ESP32 Arduino framework APIs (WiFi.h, time.h/
// configTime()/getLocalTime()) -- unlike the third-party library API
// guesses flagged elsewhere in this codebase (TJpg_Decoder, ESP32-
// audioI2S seek support), these are part of the framework itself and
// already compiled into this project's build (see the WiFi.cpp etc.
// object files in any build log), so there's no new dependency and
// higher confidence in the API surface being correct.
namespace TimeSync {

void begin(); // starts the background sync task; call once from setup()
bool isSynced();

// "HH:MM" in the configured UTC offset (state.utcOffsetHours, Settings'
// "Time zone" row), or "--:--" if never synced yet.
String currentTimeString();

} // namespace TimeSync
