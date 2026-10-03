#pragma once

#include <Arduino.h>

// "m:ss" formatting shared by MenuEngine (track durations) and Screens
// (now playing / progress times).
String fmtTime(float totalSec);

// Shared by AudioBridge (finding a fallback file to play) and Library
// (deciding which SD files count as tracks when scanning).
bool hasAudioExtension(const String &name);
