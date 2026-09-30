#include "Util.h"

bool hasAudioExtension(const String &name) {
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".flac") || lower.endsWith(".mp3") ||
           lower.endsWith(".wav") || lower.endsWith(".m4a") ||
           lower.endsWith(".aac");
}

String fmtTime(float totalSec) {
    if (totalSec < 0) totalSec = 0;
    int total = (int)totalSec;
    int m = total / 60;
    int s = total % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return String(buf);
}
