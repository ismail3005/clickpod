#pragma once

// Pin assignments for the ESP32-WROVER build.
// Cross-checked against the ESP32-WROVER-B/IB datasheet (Table 3: Pin
// Definitions, Table 4: Default Configuration of Strapping Pins). Still
// placeholders in the sense that they're not yet confirmed against the
// physical breadboard, but every pin below is verified to actually exist
// on the WROVER-B's 38-pin breakout and to be free for general I/O use.
//
// Pins deliberately AVOIDED and why:
//   GPIO6-11  - wired internally to the module's integrated SPI flash;
//               datasheet explicitly says not recommended for other uses.
//   GPIO16/17 - NOT broken out on WROVER-B/IB at all (used internally for
//               PSRAM). Do not assign anything here.
//   GPIO1/3   - UART0 TX/RX, used for flashing and serial monitor.
//   GPIO0     - boot-mode strapping pin (must read high for normal SPI
//               boot); avoid attaching anything that could pull it low.
//   GPIO12    - MTDI strapping pin, selects VDD_SDIO voltage at reset. If
//               anything holds it HIGH at boot (e.g. a breakout board with
//               its own onboard pull-up resistor), VDD_SDIO switches to
//               1.8V and the board fails to boot. Left unused rather than
//               risking it on a button pin, since the ANO breakout's pull
//               resistor situation isn't confirmed.
//   GPIO2, GPIO5, GPIO15 - also strapping pins (chip boot mode / U0TXD
//               print enable / SDIO slave timing) but their default states
//               don't conflict with how they're used below; kept, not
//               avoided, see inline notes.

// Shared SPI bus (display + SD card, separate CS pins per SPEC.md section 2)
#define PIN_SPI_SCK   18
#define PIN_SPI_MOSI  23
#define PIN_SPI_MISO  19

#define PIN_TFT_CS     5    // strapping pin, but only affects SDIO slave timing (unused here) — safe
#define PIN_TFT_DC     2    // strapping pin, only matters combined with GPIO0=0 (joint download mode) — safe for normal boot
#define PIN_TFT_RST    4

#define PIN_SD_CS      15   // strapping pin (MTDO/U0TXD print enable) — worst case is quieter boot log, not a boot failure

// I2S audio out to PCM5102A
#define PIN_I2S_BCLK  26
#define PIN_I2S_LRC   25
#define PIN_I2S_DOUT  22

// I2C bus (MAX17048 fuel gauge)
#define PIN_I2C_SDA   21
#define PIN_I2C_SCL   27

// Adafruit ANO rotary navigation encoder
// UP/RIGHT/CENTER/LEFT below were reassigned to match the actual physical
// wiring order (confirmed by testing each button and observing which GPIO
// fired) -- doesn't matter which SWn pad on the board maps to which
// direction, only that the constant here matches what's physically wired.
#define PIN_ANO_ENC_A      32
#define PIN_ANO_ENC_B      33
#define PIN_ANO_BTN_UP     39   // input-only, no internal pull — needs external/onboard pull resistor
#define PIN_ANO_BTN_DOWN   35   // input-only pin, fine for a button; moved off GPIO12 (boot-voltage strap risk)
#define PIN_ANO_BTN_LEFT   34   // input-only; also RTC-capable (fine if ever needed for wake)
#define PIN_ANO_BTN_RIGHT  14
#define PIN_ANO_BTN_CENTER 13   // must be a valid ext0 deep-sleep wake source -- GPIO13 is RTC-capable, OK

// Spare, unused for now: GPIO36 (input-only; candidate for a MAX17048 ALERT line later)
