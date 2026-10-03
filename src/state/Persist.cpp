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
    // btDeviceName is the single currently-active target (whatever
    // actually gets auto-resumed/reconnected to); btKnownDevices below is
    // the separate bounded "recent devices" convenience list -- see
    // AppState.h's big comment on it for why these are different things.
    state.btOn = prefs.getBool("btOn", false);
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
    bool btPending = prefs.getBool("btPending", false);
    bool timeSyncPending = prefs.getBool("tsPending", false);
    prefs.end();

    if (timeSyncPending) {
        // Mirrors the BT guard below -- added after a real bootloop in
        // the field traced to TimeSync's WiFi join path. Unlike BT
        // (which only auto-resumes if state.btOn was persisted true),
        // TimeSync::begin() runs unconditionally on EVERY boot -- so a
        // crash anywhere between markTimeSyncAttemptStarting() and
        // markTimeSyncAttemptDone() had no guard at all, and would
        // repeat on every single boot forever with zero way to recover
        // short of pulling the SD card. Skip just the first attempt this
        // boot; clearing the flag here means a genuinely-fixed bug
        // doesn't stay gated forever, and a still-broken one just means
        // one skipped attempt per boot instead of a bootloop.
        Serial.println(F("[persist] previous boot's WiFi time-sync attempt "
                          "never completed (likely crashed) -- skipping the "
                          "first sync attempt this boot."));
        state.timeSyncSkipFirstAttempt = true;
        prefs.begin(kNamespace, false);
        prefs.putBool("tsPending", false);
        prefs.end();
    }

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

void markTimeSyncAttemptStarting() {
    prefs.begin(kNamespace, false);
    prefs.putBool("tsPending", true);
    prefs.end();
}

void markTimeSyncAttemptDone() {
    prefs.begin(kNamespace, false);
    prefs.putBool("tsPending", false);
    prefs.end();
}

} // namespace Persist
