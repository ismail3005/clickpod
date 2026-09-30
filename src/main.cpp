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
// real SD-scanned library (src/ui/Library.h -- falls back to a small
// placeholder set if scanning finds nothing); selecting a track plays real
// audio via AudioBridge.
//
// Step 5: per spec section 7, wired (I2S) and Bluetooth are mutually
// exclusive OUTPUTS, but both can be "on" at the UI level now -- wired
// playback (AudioBridge) is always available, and Bluetooth is a real
// on/off toggle the user drives from the Bluetooth screen
// (MenuEngine::enterBluetooth()/exitBluetooth() call BluetoothSource::
// begin()/end() directly), not a boot-time branch anymore. BT still only
// streams a 440Hz test tone (see BluetoothSource.h) -- routing real
// decoded audio into the A2DP source instead of out to the I2S DAC is
// separate, not-yet-done work.
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
    // scan nests SD root -> playlist -> artist -> album -> file). Bumped
    // from 4MHz first to 20MHz, now to 25MHz -- confirmed reliable at
    // 20MHz on this board's wiring (a 425-track scan completed clean, no
    // corruption/retry errors in the serial log), so pushing a bit
    // further. A real library scan is dominated by FAT directory-lookup
    // latency (many small file opens) more than raw SPI throughput, so
    // this alone won't cut scan time dramatically -- if boot speed still
    // isn't good enough, the bigger win is scanning off the blocking
    // boot path entirely, which needs care around the fact that TFT_eSPI
    // and SD share this physical SPI bus (see CLAUDE.md for why that
    // hasn't been done yet). Drop back to 20MHz if this causes SD
    // errors.
    if (!SD.begin(PIN_SD_CS, SPI, 25000000, "/sd", 10)) {
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

    if (sdOk) AudioBridge::begin(audio); // wired output; Bluetooth is a separate on/off toggle driven from the UI
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

// Same reasoning as syncBatteryToUi(): the actual A2DP connection can
// change asynchronously (connecting takes a moment after begin(), and can
// drop), so this polls the real BluetoothSource state each loop()
// iteration and only marks the UI dirty when something actually changed,
// rather than the UI ever touching BluetoothSource directly.
static void syncBluetoothToUi() {
    bool running = BluetoothSource::isRunning();
    bool connected = running && BluetoothSource::isConnected();
    String connectedTo = connected ? String(BluetoothSource::kTargetDeviceName) : String("");
    if (running != state.btOn || connectedTo != state.btConnectedTo) {
        state.btOn = running;
        state.btConnectedTo = connectedTo;
        state.dirty = true;
    }
}

void loop() {
    audio.loop(); // pumps I2S streaming; must run every iteration
    AnoInput::update();
    Battery::update();
    syncBatteryToUi();
    syncBluetoothToUi();
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
