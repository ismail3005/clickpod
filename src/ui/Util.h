#pragma once

#include <Arduino.h>

// "m:ss" formatting shared by MenuEngine (track durations) and Screens
// (now playing / progress times).
String fmtTime(float totalSec);
