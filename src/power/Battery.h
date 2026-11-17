#pragma once

// Bring-up step 7: MAX17048 fuel gauge over I2C (SDA=GPIO21, SCL=GPIO27 --
// see src/config/Pins.h and docs/SPEC.md section 3.1 for the confirmed
// physical wiring). Reports battery state-of-charge %, polled at a slow
// rate since the gauge itself only updates roughly once a second and the
// value barely moves between reads.
namespace Battery {

// Starts the I2C bus and the MAX17048. Returns false if the chip didn't
// ACK on the bus (not wired up / not powered) -- callers should keep
// running with a placeholder % in that case, not halt bring-up over it.
bool begin();

// Cheap to call every loop() iteration; only actually polls the gauge
// every kPollIntervalMs internally.
void update();

bool ready();     // true once begin() succeeded
int percent();     // 0-100, last polled state-of-charge; 0 if never ready
float voltage();   // cell voltage in volts; 0 if never ready

} // namespace Battery
