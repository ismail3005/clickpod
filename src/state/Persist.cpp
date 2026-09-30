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
    bool btPending = prefs.getBool("btPending", false);
    prefs.end();

    if (btPending && state.btOn) {
        // The previous boot started a BT auto-resume attempt and never
        // confirmed it finished -- almost certainly the device crashed
        // and rebooted mid-attempt (see the header comment on load()).
        // Refuse to repeat it blind and turn this into an infinite
        // crash-reboot loop: force BT off for this boot and persist that
        // immediately so a THIRD boot doesn't see btPending=true again
        // from a stale write.
        Serial.println(F("[persist] previous boot's Bluetooth auto-resume "
                          "never completed (likely crashed) -- leaving "
                          "Bluetooth OFF this boot to avoid a reboot loop. "
                          "Turn it back on manually from the UI."));
        state.btOn = false;
        prefs.begin(kNamespace, false);
        prefs.putBool("btOn", false);
        prefs.putBool("btPending", false);
        prefs.end();
    }
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

void markBtAttemptStarting() {
    prefs.begin(kNamespace, false);
    prefs.putBool("btPending", true);
    prefs.end();
}

void markBtAttemptDone() {
    prefs.begin(kNamespace, false);
    prefs.putBool("btPending", false);
    prefs.end();
}

} // namespace Persist
