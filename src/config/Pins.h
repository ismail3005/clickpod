#pragma once

// Pin assignments for the ESP32-WROVER build.
// Values are placeholders from initial breadboard wiring — confirm/update
// against the actual breadboard before perfboard transfer (see docs/SPEC.md
// section 8 re: connectorized wiring for serviceability).

// Shared SPI bus (display + SD card, separate CS pins per SPEC.md section 2)
#define PIN_SPI_SCK   18
#define PIN_SPI_MOSI  23
#define PIN_SPI_MISO  19

#define PIN_TFT_CS     5
#define PIN_TFT_DC     2
#define PIN_TFT_RST    4

#define PIN_SD_CS      15

// I2S audio out to PCM5102A
#define PIN_I2S_BCLK  26
#define PIN_I2S_LRC   25
#define PIN_I2S_DOUT  22

// I2C bus (MAX17048 fuel gauge)
#define PIN_I2C_SDA   16
#define PIN_I2C_SCL   17

// Adafruit ANO rotary navigation encoder
#define PIN_ANO_ENC_A     32
#define PIN_ANO_ENC_B     33
#define PIN_ANO_BTN_UP    13
#define PIN_ANO_BTN_DOWN  12   // NOTE: strapping pin (MTDI) — verify no boot-mode conflict
#define PIN_ANO_BTN_LEFT  14
#define PIN_ANO_BTN_RIGHT 39
#define PIN_ANO_BTN_CENTER 34   // must be a valid ext0 deep-sleep wake source (RTC GPIO)
