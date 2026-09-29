#include <Arduino.h>
#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "config/Pins.h"
#include "input/AnoInput.h"
#include "state/AppState.h"

// Bring-up sequence (docs/SPEC.md section 4):
//   1. ESP32 + PSRAM verification            [this file]
//   2. ESP32 + SD card init                  [this file]
//   3. ESP32 + PCM5102 playback via ESP32-audioI2S [this file]
//   4. ILI9341 display alongside SD on shared SPI bus [this file]
//   5. ESP32-A2DP Bluetooth output as a separate playback path
//   6. ANO encoder + buttons                 [this file, done ahead of step 5]
//   7. MAX17048 battery monitoring
//
// This step (6) only wires up and reports raw input events (tap, long
// press, double tap, encoder rotation) to prove the encoder + 5 buttons
// are correctly wired and debounced -- deciding what each event means in
// a given UI mode is menu/screen code that doesn't exist yet (spec
// section 6). See src/input/AnoInput.h/.cpp for the actual input logic.
//
// TFT_eSPI's pin/driver config lives in platformio.ini's build_flags (not
// the library's User_Setup.h, which would get clobbered on reinstall).

static AppMode appMode = AppMode::BOOT;
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
    tft.setRotation(1); // landscape; revisit once the enclosure/UI layout is set
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.println("clickpod");
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.println("bring-up step 4: display OK");
    Serial.println(F("[bringup] Display initialized."));
}

static const char *buttonName(AnoButton button) {
    switch (button) {
        case AnoButton::UP: return "UP";
        case AnoButton::DOWN: return "DOWN";
        case AnoButton::LEFT: return "LEFT";
        case AnoButton::RIGHT: return "RIGHT";
        case AnoButton::CENTER: return "CENTER";
        default: return "?";
    }
}

// Bring-up step 6 validation: report every raw input event to serial. Not
// wired to any actual UI action yet -- see AnoInput.h for why.
static void reportAnoInput() {
    AnoInput::update();

    int16_t delta = AnoInput::takeEncoderDelta();
    if (delta != 0) {
        Serial.printf("[ano] rotate %s (delta %d)\n", delta > 0 ? "CW" : "CCW", delta);
    }

    for (uint8_t i = 0; i < static_cast<uint8_t>(AnoButton::COUNT); i++) {
        AnoButton b = static_cast<AnoButton>(i);
        if (AnoInput::wasTapped(b)) {
            Serial.printf("[ano] %s tap\n", buttonName(b));
        }
        if (AnoInput::wasLongPressed(b)) {
            Serial.printf("[ano] %s long-press\n", buttonName(b));
        }
    }

    if (AnoInput::centerWasDoubleTapped()) {
        Serial.println(F("[ano] CENTER double-tap"));
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
    initDisplay();
    // AnoInput::begin(); // TEMP disabled to isolate an audio-silence bug -- see if audio comes back without it

    bool sdOk = initSd();

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

            tft.setCursor(10, 60);
            tft.println(trackPath);
        } else {
            Serial.println(F("[bringup] No .flac/.mp3/.wav/.m4a/.aac file found on the "
                              "card -- copy a test track over to exercise I2S playback."));
        }
    }
}

void loop() {
    audio.loop();
    // reportAnoInput(); // TEMP disabled alongside AnoInput::begin() above
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
