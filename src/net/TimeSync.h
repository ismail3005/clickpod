#pragma once

#include <Arduino.h>
#include <vector>

// Grabs wall-clock time "for free" -- no RTC chip (none in the BOM).
// Scans nearby WiFi on each attempt: prefers a network matching one of the
// credentials handed to begin() (read once from an SD card text file at
// boot -- see loadCredentialsFromSd() -- deliberately kept OFF the repo,
// never hardcoded, never exposed in the UI, per the user's explicit call:
// a single physical SD card in their pocket is a much smaller exposure
// than permanent GitHub history for a repo that could go public), falling
// back to any open (no-password) network if none of the known ones are in
// range. Joins briefly, fetches NTP time, then disconnects and radios
// off, keeping time locally via millis() from then on. Re-attempts
// periodically to correct for clock drift and to cover the case where no
// usable network was found the first time.
//
// The sync loop itself runs entirely on a background FreeRTOS task (core
// 0), so it never blocks boot or normal use, and -- IMPORTANT -- that
// background task NEVER touches the SD card, only WiFi. This matters: SD
// and the TFT share one physical SPI bus, and this codebase's established
// rule (see CLAUDE.md, and why the library scan isn't backgrounded
// either) is that concurrent access to that bus from two different
// FreeRTOS tasks is NOT proven safe, only strictly sequential access from
// one task at a time is. So loadCredentialsFromSd() is called exactly
// ONCE, synchronously, from the main/setup() thread at boot (same
// sequential pattern as the library index build) -- its result is hung
// onto begin() as a plain in-RAM vector, and the background task only
// ever reads that already-parsed vector afterward, never the SD card
// directly. Beyond SD, this still carries no other cross-peripheral risk
// -- WiFi itself doesn't touch the SPI bus. It DOES share the ESP32's
// single radio with classic Bluetooth, though -- a scan+sync attempt may
// cause brief BT audio glitches if BT is actively streaming at that
// moment; not worked around given BT playback is currently just a test
// tone anyway (see BluetoothSource.h), worth revisiting once BT streams
// real audio.
//
// Depends only on core ESP32 Arduino framework APIs (WiFi.h, time.h/
// configTime()/getLocalTime()) -- unlike the third-party library API
// guesses flagged elsewhere in this codebase (TJpg_Decoder, ESP32-
// audioI2S seek support), these are part of the framework itself and
// already compiled into this project's build (see the WiFi.cpp etc.
// object files in any build log), so there's no new dependency and
// higher confidence in the API surface being correct.
namespace TimeSync {

struct WifiCredential {
    String ssid;
    String password;
};

// Reads path (SD root, e.g. "/clickpod_wifi.txt") if it exists: one
// network per line, "SSID,PASSWORD" (split on the FIRST comma only, so a
// password containing a comma still works; a line with no comma at all is
// skipped as malformed rather than guessed at). Blank lines and lines
// starting with '#' are ignored, so the user can leave themselves notes.
// Returns an empty vector if the file doesn't exist or SD isn't mounted
// -- this is an optional, best-effort file, not a hard requirement. Call
// this exactly ONCE, synchronously, from the main thread after SD is
// mounted (see the big comment above for why) -- never from inside
// TimeSync's own background task.
std::vector<WifiCredential> loadCredentialsFromSd(const char *path = "/clickpod_wifi.txt");

// Starts the background sync task; call once from setup(), after
// loadCredentialsFromSd(). credentials may be empty (falls back to
// open-network-only scanning, the original behavior).
void begin(std::vector<WifiCredential> credentials = {});
bool isSynced();

// "HH:MM" in the configured UTC offset (state.utcOffsetHours, Settings'
// "Time zone" row) if a real NTP sync has happened, else the manually-
// set wall-clock time (see setManualTime()) if one's been entered, else
// "--:--".
String currentTimeString();

// Manual "Set Time" fallback (MenuEngine::confirmSetTime(), the
// AppMode::SET_TIME screen) for when no open/known WiFi network is ever
// in range to get a real NTP sync from. Deliberately HOUR:MINUTE only,
// no date -- there's no RTC and no date entry UI, and nothing in this
// app's own UI (the statusbar clock, the AOD screen) ever displays a
// date, only HH:MM, so a date has no actual consumer to justify the
// extra UI complexity of entering one. Counts forward from millis() the
// same way a real sync does, just anchored to a user-entered time
// instead of an NTP-fetched one -- drifts at the same rate a real sync
// would between corrections, and (same as a real sync) resets to
// whatever was last entered on every reboot, since there's no RTC to
// carry wall-clock time across a power cycle either way. A real NTP
// sync completing after this takes over automatically (currentTimeString()
// always prefers `synced` when true) -- this is purely a fallback for
// when one hasn't happened (yet, or ever).
void setManualTime(int hour, int minute);
bool hasManualTime();

} // namespace TimeSync
