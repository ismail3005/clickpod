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
void load();

void save();

} // namespace Persist
