#include "Battery.h"

#include <Adafruit_MAX1704X.h>
#include <Arduino.h>
#include <Wire.h>

#include "../config/Pins.h"

namespace Battery {
namespace {

constexpr uint32_t kPollIntervalMs = 2000; // the gauge itself settles slowly; no need to hammer it

Adafruit_MAX17048 gauge;
bool gaugeReady = false;
int lastPercent = 0;
float lastVoltage = 0;
uint32_t lastPollMs = 0;

} // namespace

bool begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    if (!gauge.begin(&Wire)) {
        Serial.println(F("[battery] FAIL: MAX17048 didn't ACK on I2C. Check wiring "
                          "(SDA=GPIO21, SCL=GPIO27, VIN=3.3V, GND) and that the "
                          "battery is connected -- see docs/SPEC.md section 3.1."));
        gaugeReady = false;
        return false;
    }

    Serial.printf("[battery] MAX17048 OK, chip version 0x%04X\n", gauge.getChipID());
    gaugeReady = true;
    lastPollMs = millis();
    // First reading immediately so callers don't see a stale 0% until the
    // first poll interval elapses.
    lastPercent = (int)constrain(gauge.cellPercent(), 0.0f, 100.0f);
    lastVoltage = gauge.cellVoltage();
    return true;
}

void update() {
    if (!gaugeReady) return;
    uint32_t now = millis();
    if (now - lastPollMs < kPollIntervalMs) return;
    lastPollMs = now;

    lastPercent = (int)constrain(gauge.cellPercent(), 0.0f, 100.0f);
    lastVoltage = gauge.cellVoltage();
}

bool ready() { return gaugeReady; }
int percent() { return lastPercent; }
float voltage() { return lastVoltage; }

} // namespace Battery
