# Build-time patch for ESP32-A2DP's hardcoded 10-second connect delay.
#
# Real root cause of "Bluetooth connecting slowness" (user's own report,
# "i want to know why and fix if possible"): confirmed by reading the real
# pinned library source (github.com/pschatzmann/ESP32-A2DP, commit
# 35bace5, src/BluetoothA2DPSource.cpp's av_hdl_stack_evt(),
# BT_APP_EVT_STACK_UP case) -- EVERY single begin()/start() call, whether
# it's the first "Bluetooth On" after boot, a device-picker connect, or a
# reconnect, runs an unconditional `delay_ms(10000)` -- a full 10 seconds
# of literally nothing happening -- BEFORE the library even decides
# whether to reconnect-by-address or start a discovery scan, and before
# the heart-beat timer that drives either of those even starts. This is
# not network/radio time, not something our own code's RadioLock/heap
# guards could ever see or shorten -- it's a plain blocking sleep baked
# into the library's own stack-up handler, running on the BT stack's own
# task (bt_app_task), with no public API to configure or skip it.
#
# The comment directly above it in the real source:
#   /* Avoid the state error of s_a2d_state caused by the connection
#      initiated by the peer device. */
#   // esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
#   delay_ms(10000);
#   set_scan_mode_connectable(false);
# -- i.e. the library wants a window where the device ISN'T yet
# connectable, so a peer trying to connect mid-init can't land in the
# middle of our own state-machine setup and confuse s_a2d_state. The real
# scan-mode-disable call that would actually enforce that is commented
# out entirely -- what's left is just blindly sleeping instead, and 10
# full seconds is a deliberately generous, round-number guess, not a
# value tied to any measured requirement.
#
# What this patch does: shortens that sleep to 500ms instead of removing
# it outright -- preserves the library's own stated intent (SOME window
# before scan-mode/reconnect logic runs, in case the race it describes is
# real) while cutting the dominant cost of every single connect attempt
# by ~9.5 seconds. Picked as a reasoned, conservative guess at "clearly
# long enough to let esp_a2d_source_init()/esp_avrc_ct_init() finish their
# own internal async setup, nowhere near as long as a flat 10s" -- NOT
# independently verified against any documented minimum (there isn't one
# in the source), same honest confidence caveat as every other library-
# internals patch in this project.
#
# Why a build script, not a fork (yet): this project already has a real
# fork for ESP32-audioI2S (github.com/ismail3005/esp32-audioi2s) because
# the user forked it themselves and handed over the URL -- no equivalent
# fork of ESP32-A2DP exists yet, and this session's GitHub access doesn't
# extend to creating one unprompted. A build-time patch (same technique
# this project used for the FLAC maxFrameSize fix before it became a real
# fork commit) keeps depending on the clean upstream-pinned commit in
# platformio.ini and needs no extra permissions. If this turns out to
# help, worth asking the user to fork ESP32-A2DP the same way, same
# reasoning as the audioI2S precedent.
#
# NOT verified on real hardware -- no pio run in this sandbox. Verify on
# the next real flash: does "Bluetooth On" (and a reconnect after a
# disconnect) now land meaningfully faster, closer to ~0.5s of dead time
# instead of ~10s, with no new instability (if a peer connecting mid-init
# really was a real risk this window guards against, shortening it is the
# first thing to suspect if a NEW, different connection issue shows up
# that wasn't there before).

Import("env")
import os

ORIGINAL = """      // esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
      // ESP_BT_NON_DISCOVERABLE);
      delay_ms(10000);
      set_scan_mode_connectable(false);"""

PATCHED = """      // esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
      // ESP_BT_NON_DISCOVERABLE);
      // clickpod patch (scripts/patch_a2dp_startup_delay.py): shortened
      // from the stock 10000ms -- see that script for the full writeup
      // on why this was the real root cause of "Bluetooth connecting
      // slowness" and why 500ms, not 0, was kept here.
      delay_ms(500);
      set_scan_mode_connectable(false);"""

MARKER = "clickpod patch (scripts/patch_a2dp_startup_delay.py)"


def find_source_file():
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    pioenv = env.subst("$PIOENV")
    search_root = os.path.join(libdeps_dir, pioenv)
    if not os.path.isdir(search_root):
        return None
    for root, _dirs, files in os.walk(search_root):
        if "BluetoothA2DPSource.cpp" in files:
            return os.path.join(root, "BluetoothA2DPSource.cpp")
    return None


def patch_startup_delay():
    path = find_source_file()
    if not path:
        print("[clickpod patch] ESP32-A2DP BluetoothA2DPSource.cpp not found "
              "under PROJECT_LIBDEPS_DIR yet -- will retry next build once "
              "the library is installed (first build only)")
        return

    with open(path, "r") as f:
        content = f.read()

    if MARKER in content:
        print("[clickpod patch] A2DP startup-delay patch already applied to " + path)
        return

    if ORIGINAL not in content:
        print("[clickpod patch] WARNING: expected A2DP startup-delay code "
              "not found in " + path + " -- library source may have "
              "changed, patch NOT applied. See "
              "scripts/patch_a2dp_startup_delay.py.")
        return

    content = content.replace(ORIGINAL, PATCHED)
    with open(path, "w") as f:
        f.write(content)
    print("[clickpod patch] A2DP startup-delay patch applied to " + path)


patch_startup_delay()
