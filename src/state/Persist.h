#pragma once

// Persists user settings (brightness, dark mode, sort preference, time
// zone) and whether Bluetooth was left on across reboots, using the
// ESP32's built-in NVS flash storage (via the core Arduino `Preferences`
// library -- not a third-party dependency, already part of this
// project's build like WiFi.h/TimeSync's APIs).
//
// No debouncing/batching: save() does an immediate NVS write, called
// directly from each setting's change site (Settings' slider/choice
// setters, the Bluetooth on/off actions). Settings change rarely enough
// (occasional user taps, not a hot loop) that this is simpler and safer
// than trying to batch writes, and NVS's wear-leveling handles far more
// frequent writes than this will ever produce.
namespace Persist {

// Reads saved values into `state`, falling back to state's existing
// defaults for anything never saved (first boot). Call once from
// setup(), before UI::begin() so the initial screens reflect saved
// settings immediately.
//
// Boot-crash guard for Bluetooth auto-resume: if `state.btOn` was true on
// the last save, main.cpp auto-starts Bluetooth again in setup(). If BT
// itself crashes on start (a real issue hit in the field -- see
// CLAUDE.md's "Sixth/Seventh real hardware bug" writeups), that crash
// happens INSIDE that same setup() call, before anything can persist
// that it failed -- so without a guard, every reboot repeats the exact
// same auto-resume-then-crash sequence forever, and the device never
// reaches a usable state again (this is what "playback stopped working
// for any song" actually was -- the device was stuck rebooting before
// ever reaching loop()). load() detects that the previous boot's BT
// attempt never confirmed completion and forces `state.btOn = false`
// for this boot instead of repeating it -- see markBtAttemptStarting()/
// markBtAttemptDone() below.
void load();

void save();

// Call immediately before BluetoothSource::begin() in setup(), only when
// about to auto-resume BT because state.btOn was persisted true. Writes
// a "pending" flag to NVS synchronously.
void markBtAttemptStarting();

// Call immediately after BluetoothSource::begin() returns (however it
// went -- success, decline, whatever) to clear the pending flag. If the
// device crashes between markBtAttemptStarting() and this call, the flag
// survives into the next boot and load() will see it and skip resuming.
void markBtAttemptDone();

} // namespace Persist
