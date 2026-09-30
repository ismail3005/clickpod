#include "Persist.h"

#include <Preferences.h>

#include "AppState.h"

namespace Persist {
namespace {

Preferences prefs;
constexpr const char *kNamespace = "clickpod";

} // namespace

void load() {
    prefs.begin(kNamespace, true); // read-only
    state.brightness = prefs.getInt("brightness", state.brightness);
    state.darkMode = prefs.getBool("darkMode", state.darkMode);
    state.sortPref = prefs.getString("sortPref", state.sortPref).c_str();
    state.utcOffsetHours = prefs.getInt("utcOffset", state.utcOffsetHours);
    // Whether Bluetooth was left on -- main.cpp resumes it at boot if so.
    // There's only ever one configured target device right now
    // (BluetoothSource::kTargetDeviceName), so there's no per-device
    // "paired devices" list to persist yet -- if/when the UI supports
    // choosing a different target device, that name belongs here too.
    state.btOn = prefs.getBool("btOn", false);
    prefs.end();
}

void save() {
    prefs.begin(kNamespace, false); // read-write
    prefs.putInt("brightness", state.brightness);
    prefs.putBool("darkMode", state.darkMode);
    prefs.putString("sortPref", state.sortPref.c_str());
    prefs.putInt("utcOffset", state.utcOffsetHours);
    prefs.putBool("btOn", state.btOn);
    prefs.end();
}

} // namespace Persist
