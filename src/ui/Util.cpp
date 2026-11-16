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

namespace {

// U+00C0..U+00FF (Latin-1 letters), in order.
const char *const kLatin1Fold[64] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",
};

const char *foldCodepoint(uint32_t cp) {
    if (cp >= 0xC0 && cp <= 0xFF) return kLatin1Fold[cp - 0xC0];
    switch (cp) {
        case 0x00A0: return " ";  // no-break space
        case 0x00A1: return "!";
        case 0x00AB: case 0x00BB: return "\"";
        case 0x00B4: return "'";
        case 0x00B7: return ".";
        case 0x00BF: return "?";
        case 0x0131: return "i";  // dotless i
        case 0x0141: return "L";
        case 0x0142: return "l";
        case 0x0152: return "OE";
        case 0x0153: return "oe";
        case 0x0160: return "S";
        case 0x0161: return "s";
        case 0x0178: return "Y";
        case 0x017D: return "Z";
        case 0x017E: return "z";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
        case 0x2022: return "*";
        case 0x2026: return "...";
        default: return "?";
    }
}

} // namespace

String asciiFold(const String &in) {
    const char *s = in.c_str();
    bool ascii = true;
    for (const char *p = s; *p; p++) {
        if ((uint8_t)*p >= 0x80) { ascii = false; break; }
    }
    if (ascii) return in;

    String out;
    out.reserve(in.length());
    while (*s) {
        uint8_t c = (uint8_t)*s++;
        if (c < 0x80) { out += (char)c; continue; }
        uint32_t cp;
        int extra;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out += '?'; continue; } // stray continuation byte / not UTF-8
        bool ok = true;
        for (int i = 0; i < extra; i++) {
            if (((uint8_t)*s & 0xC0) != 0x80) { ok = false; break; } // also stops at the terminating NUL
            cp = (cp << 6) | ((uint8_t)*s & 0x3F);
            s++;
        }
        out += ok ? foldCodepoint(cp) : "?";
    }
    return out;
}
