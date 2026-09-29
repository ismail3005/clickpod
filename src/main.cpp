#include <Arduino.h>
#include <Audio.h>
#include <SD.h>
#include <SPI.h>

#include "config/Pins.h"
#include "state/AppState.h"

// Bring-up sequence (docs/SPEC.md section 4):
//   1. ESP32 + PSRAM verification            [this file]
//   2. ESP32 + SD card file listing over serial [this file]
//   3. ESP32 + PCM5102 playback via ESP32-audioI2S [this file]
//   4. ILI9341 display alongside SD on shared SPI bus
//   5. ESP32-A2DP Bluetooth output as a separate playback path
//   6. ANO encoder + buttons
//   7. MAX17048 battery monitoring
//
// This file currently implements steps 1-3 only. Later steps get their own
// modules under src/ (ui/, input/, bt/, power/) as they're brought up.

static AppMode appMode = AppMode::BOOT;
static Audio audio;

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

static bool hasAudioExtension(const String &name) {
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".flac") || lower.endsWith(".mp3") ||
           lower.endsWith(".wav") || lower.endsWith(".m4a") ||
           lower.endsWith(".aac");
}

// Recursively searches for the first playable, actually-openable audio file
// on the card, so bring-up doesn't depend on a particular library layout
// being present yet. entry.name() only ever returns the bare filename (not
// the path from root), so the caller-supplied dirPath has to be threaded
// through the recursion to build a real absolute path for nested files --
// almost everything on a real library is Artist/Album/track.flac, not
// sitting at the root. Candidates are also verified with SD.exists() before
// being accepted: a file can be listed but still fail to open by that exact
// path (e.g. non-ASCII punctuation in the name that doesn't round-trip
// through the filesystem the same way twice) -- better to skip to the next
// track than hand the audio library a path we already know won't resolve.
static bool findFirstAudioFile(File dir, const String &dirPath, String &outPath) {
    while (File entry = dir.openNextFile()) {
        String path = dirPath + "/" + entry.name();

        if (entry.isDirectory()) {
            bool found = findFirstAudioFile(entry, path, outPath);
            entry.close();
            if (found) return true;
        } else {
            if (hasAudioExtension(path) && SD.exists(path)) {
                outPath = path;
                entry.close();
                return true;
            }
            entry.close();
        }
    }
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=== DIY iPod-Classic MP3 Player - bring-up build ==="));

    verifyPsram();

    bool sdOk = initSd();
    if (sdOk) {
        Serial.println(F("[bringup] SD contents:"));
        File root = SD.open("/");
        listDir(root, 0);
        root.close();
    }

    appMode = AppMode::MENU;

    if (sdOk) {
        String trackPath;
        File root = SD.open("/");
        bool found = findFirstAudioFile(root, "", trackPath);
        root.close();

        if (found) {
            Serial.printf("[bringup] Playing first audio file found: %s\n", trackPath.c_str());
            audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
            audio.setVolume(10); // 0-21; start low, raise once confirmed working
            audio.connecttoFS(SD, trackPath.c_str());
            appMode = AppMode::NOW_PLAYING;
        } else {
            Serial.println(F("[bringup] No .flac/.mp3/.wav/.m4a/.aac file found on the "
                              "card -- copy a test track over to exercise I2S playback."));
        }
    }
}

void loop() {
    audio.loop();
}

// ESP32-audioI2S optional callbacks -- useful during bring-up to see what
// the library actually parsed out of the file.
void audio_info(const char *info) {
    Serial.printf("[audio] info: %s\n", info);
}

void audio_id3data(const char *info) {
    Serial.printf("[audio] id3/metadata: %s\n", info);
}

void audio_eof_mp3(const char *info) {
    Serial.printf("[audio] end of file: %s\n", info);
}
