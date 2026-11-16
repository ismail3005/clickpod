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
    // btDeviceName is the single currently-bonded target; btKnownDevices
    // below is the separate bounded "recent devices" convenience list --
    // see AppState.h's big comment on it for why these are different
    // things. Whether Bluetooth was ON is not persisted (see Persist.h).
    state.btDeviceName = prefs.getString("btDeviceName", "").c_str();
    // Recent-devices list: stored as one newline-joined string (NVS/
    // Preferences has no native array type), split back out here. Empty
    // string -> empty list, same as a fresh/never-saved device.
    {
        String joined = prefs.getString("btKnown", "").c_str();
        state.btKnownDevices.clear();
        int start = 0;
        while (start < (int)joined.length()) {
            int nl = joined.indexOf('\n', start);
            if (nl < 0) nl = joined.length();
            if (nl > start) state.btKnownDevices.push_back(joined.substring(start, nl));
            start = nl + 1;
        }
    }
    bool timeSyncPending = prefs.getBool("tsPending", false);
    prefs.end();

    if (timeSyncPending) {
        // Added after a real bootloop in the field traced to TimeSync's
        // WiFi join path: TimeSync::begin() runs unconditionally on EVERY
        // boot, so a crash anywhere between markTimeSyncAttemptStarting()
        // and markTimeSyncAttemptDone() would otherwise repeat on every
        // single boot forever. Skip just the first attempt this boot;
        // clearing the flag here means a genuinely-fixed bug doesn't stay
        // gated forever, and a still-broken one just means one skipped
        // attempt per boot instead of a bootloop.
        Serial.println(F("[persist] previous boot's WiFi time-sync attempt "
                          "never completed (likely crashed) -- skipping the "
                          "first sync attempt this boot."));
        state.timeSyncSkipFirstAttempt = true;
        prefs.begin(kNamespace, false);
        prefs.putBool("tsPending", false);
        prefs.end();
    }
}

void save() {
    prefs.begin(kNamespace, false); // read-write
    prefs.putInt("brightness", state.brightness);
    prefs.putBool("darkMode", state.darkMode);
    prefs.putString("sortPref", state.sortPref.c_str());
    prefs.putInt("utcOffset", state.utcOffsetHours);
    prefs.putString("btDeviceName", state.btDeviceName.c_str());
    {
        String joined;
        for (size_t i = 0; i < state.btKnownDevices.size(); i++) {
            if (i) joined += '\n';
            joined += state.btKnownDevices[i];
        }
        prefs.putString("btKnown", joined.c_str());
    }
    prefs.end();
}

// Own Preferences handle on purpose -- see Persist.h. These run on
// TimeSync's task while save() may be running on the main loop.
void markTimeSyncAttemptStarting() {
    Preferences p;
    p.begin(kNamespace, false);
    p.putBool("tsPending", true);
    p.end();
}

void markTimeSyncAttemptDone() {
    Preferences p;
    p.begin(kNamespace, false);
    p.putBool("tsPending", false);
    p.end();
}

} // namespace Persist
