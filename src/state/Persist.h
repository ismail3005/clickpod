#pragma once

// Persists user settings (brightness, dark mode, sort preference, time
// zone) and the remembered Bluetooth device names across reboots, using
// the ESP32's built-in NVS flash storage (via the core Arduino
// `Preferences` library -- not a third-party dependency).
//
// No debouncing/batching: save() does an immediate NVS write, called
// directly from each setting's change site (Settings' slider/choice
// setters, the Bluetooth device actions). Settings change rarely enough
// (occasional user taps, not a hot loop) that this is simpler and safer
// than trying to batch writes, and NVS only rewrites keys whose value
// actually changed.
//
// Whether Bluetooth was ON is deliberately NOT persisted: Bluetooth never
// auto-resumes at boot (user's explicit call, see CLAUDE.md's
// twenty-seventh hardware bug), so there is nothing to restore.
namespace Persist {

// Reads saved values into `state`, falling back to state's existing
// defaults for anything never saved (first boot). Call once from
// setup(), before UI::begin() so the initial screens reflect saved
// settings immediately.
void load();

void save();

// Boot-crash guard for TimeSync's WiFi join path -- added after a real
// bootloop traced to it (see CLAUDE.md). TimeSync runs on EVERY boot
// unconditionally, so without this any crash in the join path repeats
// forever with no recovery path. Call starting() immediately before the
// risky WiFi.begin() call inside TimeSync::tryOnce(), done() immediately
// after the connect attempt resolves (success or ordinary failure). If
// the device dies in between, load() sees the flag on the next boot and
// has TimeSync skip that boot's first attempt.
//
// These two are called from TimeSync's own background task, not the main
// loop -- they use their own Preferences handle, never the one load()/
// save() share (a Preferences object isn't safe to begin()/end() from two
// tasks at once; NVS itself is).
void markTimeSyncAttemptStarting();
void markTimeSyncAttemptDone();

} // namespace Persist
