#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "config/Pins.h"
#include "state/AppState.h"

// Bring-up sequence (docs/SPEC.md section 4):
//   1. ESP32 + PSRAM verification            [this file]
//   2. ESP32 + SD card file listing over serial [this file]
//   3. ESP32 + PCM5102 playback via ESP32-audioI2S
//   4. ILI9341 display alongside SD on shared SPI bus
//   5. ESP32-A2DP Bluetooth output as a separate playback path
//   6. ANO encoder + buttons
//   7. MAX17048 battery monitoring
//
// This file currently implements steps 1-2 only. Later steps get their own
// modules under src/ (audio/, ui/, input/, bt/, power/) as they're brought up.

static AppMode appMode = AppMode::BOOT;

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

static void listDir(File dir, uint8_t depth) {
    while (File entry = dir.openNextFile()) {
        for (uint8_t i = 0; i < depth; i++) Serial.print("  ");
        if (entry.isDirectory()) {
            Serial.printf("[DIR]  %s\n", entry.name());
            listDir(entry, depth + 1);
        } else {
            Serial.printf("       %s (%u bytes)\n", entry.name(), entry.size());
        }
        entry.close();
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=== DIY iPod-Classic MP3 Player - bring-up build ==="));

    verifyPsram();

    if (initSd()) {
        Serial.println(F("[bringup] SD contents:"));
        File root = SD.open("/");
        listDir(root, 0);
        root.close();
    }

    appMode = AppMode::MENU;
    Serial.println(F("[bringup] Steps 1-2 complete. Next: wire PCM5102A and bring up "
                      "ESP32-audioI2S playback (step 3)."));
}

void loop() {
    // Nothing yet — playback, display, input, BT, and battery monitoring
    // are added in subsequent bring-up steps.
}
