#include <Arduino.h>
#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "audio/AudioBridge.h"
#include "bt/BluetoothSource.h"
#include "config/Pins.h"
#include "input/AnoInput.h"
#include "power/Battery.h"
#include "state/AppState.h"
#include "ui/Library.h"
#include "ui/UI.h"

// Bring-up sequence (docs/SPEC.md section 4):
//   1. ESP32 + PSRAM verification            [this file]
//   2. ESP32 + SD card init                  [this file]
//   3. ESP32 + PCM5102 playback via ESP32-audioI2S [this file]
//   4. ILI9341 display alongside SD on shared SPI bus [this file]
//   5. ESP32-A2DP Bluetooth output, tested in isolation [this file]
//   6. ANO encoder + buttons                 [this file, done ahead of step 5]
//   7. MAX17048 battery monitoring           [src/power/Battery.*]
//
// Steps 1-6 are hardware-confirmed (see README). The real UI/UX layer
// (menus, now playing, lyrics, queue, Bluetooth screen -- docs/SPEC.md
// section 6) is ported from the browser simulator into src/ui/, src/state/,
// and src/audio/ -- see UI::begin()/UI::update() below. It navigates a
// placeholder mock library (src/ui/Library.h) since real FLAC metadata
// parsing is still an open risk (spec section 10); selecting a track plays
// real audio via AudioBridge (whatever file it finds first on the card),
// so DAC output is real even though the on-screen metadata isn't matched
// to the specific file yet.
//
// Step 5: per spec section 7, wired (I2S) and Bluetooth are mutually
// exclusive output paths, manually switched by the user -- never run both
// at once. kTestWiredPlayback below picks which one this build exercises;
// SD/display/ANO/UI stay active either way since none of those conflict
// with the choice of audio output. AudioBridge (the UI's real-playback
// hook) is only wired up in the wired path -- BT bring-up still runs its
// own isolated test tone via BluetoothSource, so selecting a track from
// the UI in that mode won't produce sound, only navigate.
constexpr bool kTestWiredPlayback = true;
// In A2DP SOURCE mode this is the name of the target SINK device to scan
// for and auto-connect to (e.g. your headphones/speaker) -- NOT the
// ESP32's own advertised name. Source actively seeks out a known sink by
// name, the reverse of how a peripheral you'd pair to from a phone's
// Bluetooth settings works. Put your headphones/speaker's exact BT name
// here and make sure they're in pairing/discoverable mode when this runs.
constexpr const char *kBtDeviceName = "ULT WEAR";
//
// TFT_eSPI's pin/driver config lives in platformio.ini's build_flags (not
// the library's User_Setup.h, which would get clobbered on reinstall).

static Audio audio;
static TFT_eSPI tft = TFT_eSPI();

static void verifyPsram() {
    Serial.println(F("[bringup] Checking PSRAM..."));
    if (psramFound()) {
        Serial.printf("[bringup] PSRAM OK: %u bytes\n", ESP.getPsramSize());
    } else {
        Serial.println(F("[bringup] FAIL: no PSRAM detected. Check board_build.flash_mode "
                          "and BOARD_HAS_PSRAM build flag, and that this is a WROVER (not WROOM)."));
    }
}

static bool initSd() {
    Serial.println(F("[bringup] Initializing SD card..."));
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);
    // SD.begin()'s defaults are conservative for a real library scan: a
    // plain SD.begin(cs) uses 4MHz SPI and a 5-file handle limit, both
    // fine for the original bring-up test (play one file) but painfully
    // slow and tight once Library::scanFromSd() is walking hundreds of
    // files with up to ~5 directories open at once (a playlist-folder
    // scan nests SD root -> playlist -> artist -> album -> file). 20MHz
    // is a safe step up from 4MHz for typical breadboard/jumper SD
    // wiring (this board's TFT already runs its SPI bus at 40MHz, but
    // that's a much shorter/cleaner trace); raise further if reliable.
    if (!SD.begin(PIN_SD_CS, SPI, 20000000, "/sd", 10)) {
        Serial.println(F("[bringup] FAIL: SD.begin() failed. Check wiring/CS pin and "
                          "that the card is FAT32-formatted."));
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println(F("[bringup] FAIL: no SD card detected."));
        return false;
    }

    uint64_t cardSizeMB = SD.cardSize() / (1024 * 1024);
    Serial.printf("[bringup] SD OK: %llu MB\n", cardSizeMB);
    return true;
}

// TFT_eSPI opens its own SPI transaction per call, so it coexists fine with
// SD sharing the same physical bus as long as each uses its own CS pin --
// nothing else to coordinate here, no shared SPI.begin() bookkeeping needed.
static void initDisplay() {
    Serial.println(F("[bringup] Initializing display..."));
    tft.init();
    tft.setRotation(1); // landscape; UI is built for 320x240
    tft.fillScreen(TFT_BLACK);
    Serial.println(F("[bringup] Display initialized."));
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=== clickpod firmware ==="));

    verifyPsram();
    initDisplay();
    AnoInput::begin();

    bool sdOk = initSd();

    // UI::begin() draws the boot splash immediately -- do this BEFORE the
    // (potentially slow, on a large card) SD library scan below, so there's
    // visual proof-of-life on screen right away instead of a black screen
    // for however long the scan takes.
    UI::begin(tft);

    if (sdOk) {
        // Visual proof that this is a slow scan in progress, not a hang --
        // easy to mistake for one otherwise on a large library (this is
        // literally what happened during bring-up). Plain direct tft
        // prints, same as the original step-1-6 bring-up status lines --
        // no need to route this through the UI state machine for a
        // one-off message drawn once before real UI::update() calls start.
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(10, 220);
        tft.print("Scanning library...");
        Library::scanFromSd(); // replaces the placeholder library if it finds any real tracks
    }

    // Non-fatal if the gauge doesn't ACK (e.g. bench-testing with the
    // battery disconnected) -- the UI just keeps showing its placeholder
    // battery % instead of a live reading, same "don't halt bring-up over
    // a single subsystem" approach as SD/BT above.
    Battery::begin();

    if (kTestWiredPlayback) {
        if (sdOk) AudioBridge::begin(audio);
    } else {
        BluetoothSource::begin(kBtDeviceName); // UI still runs for navigation; no AudioBridge wiring in this mode
    }
}

// Pushes the fuel gauge's latest reading into the UI's state, only marking
// the screen dirty when the displayed % actually changes -- polling itself
// is throttled inside Battery::update(), this just avoids redundant
// redraws on every loop() iteration in between polls.
static void syncBatteryToUi() {
    if (!Battery::ready()) return;
    int pct = Battery::percent();
    if (pct != state.battery) {
        state.battery = pct;
        state.dirty = true;
    }
}

void loop() {
    if (kTestWiredPlayback) audio.loop(); // pumps I2S streaming; must run every iteration
    AnoInput::update();
    Battery::update();
    syncBatteryToUi();
    UI::update();
}

// ESP32-audioI2S optional callbacks -- useful to see what the library
// actually parsed out of the file.
void audio_info(const char *info) {
    Serial.printf("[audio] info: %s\n", info);
}

void audio_id3data(const char *info) {
    Serial.printf("[audio] id3/metadata: %s\n", info);
}

void audio_eof_mp3(const char *info) {
    Serial.printf("[audio] end of file: %s\n", info);
}
