#include <Arduino.h>
#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "audio/AudioBridge.h"
#include "bt/BluetoothSource.h"
#include "config/Pins.h"
#include "input/AnoInput.h"
#include "state/AppState.h"
#include "ui/UI.h"

// Bring-up sequence (docs/SPEC.md section 4):
//   1. ESP32 + PSRAM verification            [this file]
//   2. ESP32 + SD card init                  [this file]
//   3. ESP32 + PCM5102 playback via ESP32-audioI2S [this file]
//   4. ILI9341 display alongside SD on shared SPI bus [this file]
//   5. ESP32-A2DP Bluetooth output, tested in isolation [this file]
//   6. ANO encoder + buttons                 [this file, done ahead of step 5]
//   7. MAX17048 battery monitoring
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
    if (!SD.begin(PIN_SD_CS)) {
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

    UI::begin(tft);
    if (kTestWiredPlayback) {
        if (sdOk) AudioBridge::begin(audio);
    } else {
        BluetoothSource::begin(kBtDeviceName); // UI still runs for navigation; no AudioBridge wiring in this mode
    }
}

void loop() {
    if (kTestWiredPlayback) audio.loop(); // pumps I2S streaming; must run every iteration
    AnoInput::update();
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
