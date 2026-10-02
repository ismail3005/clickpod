#include <Arduino.h>
#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <utility>

#include "audio/AudioBridge.h"
#include "bt/BluetoothSource.h"
#include "config/Pins.h"
#include "input/AnoInput.h"
#include "net/TimeSync.h"
#include "power/Battery.h"
#include "state/AppState.h"
#include "state/Persist.h"
#include "ui/Library.h"
#include "ui/Screens.h"
#include "ui/UI.h"

// Real, verified Arduino-ESP32 core mechanism (confirmed by reading the
// actual core/esp32/main.cpp source, not guessed): getArduinoLoopTaskStackSize()
// is declared __attribute__((weak)) there specifically so a sketch can
// override it to resize the loop() task's stack -- the default is a plain
// 8192 bytes (ARDUINO_LOOP_STACK_SIZE). Added in response to a real crash:
// `assert failed: spinlock_acquire spinlock.h:122` -- a multi-core heap-
// lock assert, the classic signature of memory corruption near a task's
// stack (NOT a heap-size failure the radioHeapOk() guards already cover;
// this is a different failure class). It hit immediately after scrubbing
// (a real seek, triggering FLACDecoderReset()) while Bluetooth was
// actively streaming real audio -- a call-depth combination that didn't
// exist in any earlier testing: decode/resync -> Audio::playChunk() ->
// audio_process_i2s() -> BluetoothSource::feedPcm()'s bounded blocking
// retry loop, all nested several frames deep inside the SAME loop()-task
// call chain that also does menu rendering, SD/FlacMeta reads, and
// Library index lookups elsewhere. 8192 bytes was never tested against
// that specific worst-case depth. This is a reasoned hypothesis from the
// failure signature and the fact this is a genuinely new code path, not
// a confirmed root cause -- flagged honestly in CLAUDE.md. Bumped by 8KB
// (not further) to stay mindful of this project's hard-won internal-heap
// headroom (the thirtieth hardware bug's PSRAM-threshold work) -- stack
// is a one-time static allocation at boot, not a per-operation cost, so
// 8KB more here is a bounded, known tradeoff, unlike BT/WiFi's dynamic
// heap pressure.
size_t getArduinoLoopTaskStackSize(void) { return 16384; }

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
    // slow and tight once Library::ensureIndex() is walking hundreds of
    // files with up to ~5 directories open at once (a playlist-folder
    // scan nests SD root -> playlist -> artist -> album -> file). Bumped
    // from 4MHz first to 20MHz, now to 25MHz -- confirmed reliable at
    // 20MHz on this board's wiring (a 425-track scan completed clean, no
    // corruption/retry errors in the serial log), so pushing a bit
    // further. Only matters for the FIRST ever boot (or after a manual
    // "Rescan library") now -- ensureIndex() skips this walk entirely on
    // every subsequent boot once the on-SD index exists (see Library.h) --
    // but still worth keeping fast for whenever it does need to run. Drop
    // back to 20MHz if this causes SD errors.
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

    // This app only ever uses classic BT (A2DP source, via ESP32-A2DP) --
    // never BLE. The Arduino-ESP32 framework's default sdkconfig enables
    // both classic BT and BLE controller memory pools, so the unused BLE
    // pool (~50KB of internal DRAM) sits reserved the whole time for
    // nothing. Releasing it here, before ANYTHING touches the BT
    // controller or WiFi, is the standard fix for exactly the symptom
    // hit in the field: WiFi failing its own rx-buffer allocation
    // ("Expected to init 4 rx buffer, actual is 0") immediately followed
    // by a Bluedroid assert crash on BT startup (`semphr_create_wrapper`/
    // `hash_map_set`, both heap-allocation failures inside the BT stack,
    // not the WiFi+BT-timing-race RadioLock.h was built to prevent --
    // that fix didn't actually stop this, see CLAUDE.md). Must run before
    // the controller is initialized -- first thing in setup(), not lazily
    // inside BluetoothSource::begin(). esp_bt.h/esp_bt_controller_mem_
    // release() are stable core ESP-IDF API (bundled with the Arduino-
    // ESP32 framework, not a new dependency) -- not independently
    // verified against the real header in this sandbox (no IDF headers
    // available here), but this is a long-standing, widely-used pattern
    // for classic-BT-only apps, not a guess at an obscure API surface.
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    Serial.printf("[bringup] free heap after BLE mem release: %u bytes total, %u internal\n",
                  ESP.getFreeHeap(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Before anything else draws/reads state -- brightness/dark mode/sort/
    // time zone/whether BT was left on all come from here if previously
    // saved (src/state/Persist.*), so the very first screen already
    // reflects them instead of a brief flash of defaults.
    Persist::load();

    verifyPsram();

    // User's explicit call: don't leave the 4MB of PSRAM this board has
    // sitting unused while internal RAM stays the scarce, crash-prone
    // resource (see CLAUDE.md's whole radio-heap-guard saga). Three of
    // this round's fixes explicitly move specific large/transient
    // buffers to PSRAM via ps_malloc() (AlbumArt's cached art buffer,
    // FlacMeta's embedded-picture read buffer) -- this is the broader
    // systemic complement: any plain malloc()/new allocation of 4KB or
    // more (a std::vector<Track> growing for a big opened playlist, a
    // large lyrics text buffer, anything else not explicitly handled)
    // now prefers PSRAM automatically instead of needing every call site
    // hunted down by hand. heap_caps_malloc_extmem_enable() is a real,
    // long-standing Arduino-ESP32 core function built for exactly this.
    // Safe for WiFi/BT/I2S's own DMA-capable buffers specifically because
    // this threshold only affects plain, capability-unspecified malloc()/
    // new calls -- code that explicitly requests MALLOC_CAP_DMA/INTERNAL
    // (which any well-behaved driver needing DMA-safe memory does) is
    // unaffected and still gets internal RAM regardless of this setting.
    // Residual honest caveat: that's the documented contract of the
    // capability-tag system, not something independently verified against
    // ESP32-A2DP/ESP32-audioI2S's internal allocation calls in this
    // sandbox -- if audio or Bluetooth output gets audibly glitchy/
    // corrupted after this (as opposed to just failing to start, which
    // the existing heap guards already handle safely), that's the first
    // thing to suspect and this call is the one to revert.
    //
    // LOWERED again, 128 -> 1 -- real logged numbers from playing a full
    // 425-track playlist (CLAUDE.md's twenty-ninth-area writeup) showed
    // Bluetooth refusing to start AT ALL after a fresh RST, free internal
    // heap stuck around ~40KB against a 60KB cold-init floor, confirming
    // the 128-byte threshold wasn't actually fixing the thing it was
    // introduced for. The reason: real Track strings on this card are
    // mostly SHORTER than 128 bytes (artist/album names, short titles --
    // even a full nested path like
    // "/funky times/Dire Straits/Making Movies/Dire Straits - Tunnel of
    // Love (Intro The Carousel Waltz).flac" is only 102 bytes), so most
    // of the per-track String allocations this was meant to catch were
    // STILL landing in internal RAM under the old 128 floor -- this
    // matters most for `state.queue`, which holds every remaining track
    // of whatever's actually playing as Track structs for the WHOLE
    // playback session (not just transiently while a menu screen is
    // open), so playing a large playlist keeps hundreds of these live in
    // RAM continuously. Dropping the floor to 1 byte means essentially
    // any non-empty String (Arduino's String has no small-string
    // optimization -- any non-empty one allocates immediately) now
    // prefers PSRAM, not just the rare large one. Same safety argument as
    // every previous lowering of this threshold: DMA-capable allocations
    // explicitly request MALLOC_CAP_DMA/INTERNAL and bypass this
    // threshold regardless, so WiFi/BT/I2S's own buffers are unaffected
    // either way -- the only real cost is PSRAM's slightly slower random
    // access vs. internal RAM, negligible for text data that's just being
    // compared/copied, not hot-looped.
    heap_caps_malloc_extmem_enable(1);

    initDisplay();
    AnoInput::begin();

    bool sdOk = initSd();
    std::vector<TimeSync::WifiCredential> wifiCreds; // stays empty if no SD / no credentials file

    // UI::begin() draws the boot splash immediately -- do this BEFORE the
    // (potentially slow, on a large card) SD library scan below, so there's
    // visual proof-of-life on screen right away instead of a black screen
    // for however long the scan takes.
    UI::begin(tft);
    Screens::applyBrightness(state.brightness); // real-hardware test, see Screens.h

    if (sdOk) {
        // Read ONCE here, synchronously, on this thread -- NOT from inside
        // TimeSync's own background task, which must never touch the SD
        // card itself (shared SPI bus with the TFT, only sequential access
        // from one task is proven safe -- see TimeSync.h's big comment and
        // CLAUDE.md). Optional file; stays an empty vector if it doesn't
        // exist, which TimeSync::begin() handles by falling back to
        // open-network-only scanning, same as before this feature existed.
        wifiCreds = TimeSync::loadCredentialsFromSd();
        // Visual proof that this might take a moment -- easy to mistake
        // for a hang otherwise (this is literally what happened during
        // bring-up). Plain direct tft prints, same as the original
        // step-1-6 bring-up status lines -- no need to route this through
        // the UI state machine for a one-off message drawn once before
        // real UI::update() calls start. In practice this is only slow on
        // the FIRST ever boot (or after a manual "Rescan library" from
        // Settings) -- ensureIndex() just checks the index file exists on
        // every boot after that and skips the FAT walk entirely, so this
        // message usually only flashes by for a moment. See Library.h/
        // CLAUDE.md for the on-SD index this replaced the old always-
        // rescan-every-boot scanFromSd() with.
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(10, 220);
        tft.print("Loading library...");
        Library::ensureIndex();
        Serial.printf("[bringup] free heap after library index: %u bytes total, %u internal\n",
                      ESP.getFreeHeap(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    // Non-fatal if the gauge doesn't ACK (e.g. bench-testing with the
    // battery disconnected) -- the UI just keeps showing its placeholder
    // battery % instead of a live reading, same "don't halt bring-up over
    // a single subsystem" approach as SD/BT above.
    Battery::begin();

    if (sdOk) AudioBridge::begin(audio); // wired output; Bluetooth is a separate on/off toggle driven from the UI

    // Bluetooth deliberately does NOT auto-resume on boot anymore, even if
    // it was left on last session (state.btOn is still persisted and still
    // drives the UI's remembered on/off display -- just no longer acted on
    // here). User's own explicit call after a real crash-reboot while BT
    // was mid-reconnect-loop: auto-resuming on every boot meant the radio
    // started hammering reconnect/discovery attempts immediately and
    // unattended, which is both a real battery cost and (combined with
    // TimeSync's periodic WiFi scans sharing the same radio) a real crash
    // surface -- "I should turn on Bluetooth manually" instead. Turning it
    // on from the Bluetooth menu still works exactly as before, including
    // the shadow-reconnect fix (reconnects to the last bonded device
    // without needing pairing mode again) and the new give-up-after-a-
    // minute watchdog (BluetoothSource::tick(), see loop() below) so a
    // manual "on" with nothing in range doesn't hammer forever either.
    // The boot-crash guard (markBtAttemptStarting/Done, Persist::load())
    // is now dead code for this specific call site but left in place --
    // enterBluetooth()'s manual "Bluetooth On" row doesn't use it (never
    // needed to, see Persist.h's comment), and it's harmless to keep for
    // whenever boot auto-resume is reconsidered.

    // Runs entirely on its own background task -- doesn't block the rest
    // of setup() or touch anything else here. See TimeSync.h.
    TimeSync::begin(std::move(wifiCreds));
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
    String connectedTo = connected ? String(BluetoothSource::currentTargetName()) : String("");
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
    BluetoothSource::tick(); // give-up-after-a-minute-of-searching watchdog, see BluetoothSource.h
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
