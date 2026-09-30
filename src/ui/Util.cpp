#include "Util.h"

String fmtTime(float totalSec) {
    if (totalSec < 0) totalSec = 0;
    int total = (int)totalSec;
    int m = total / 60;
    int s = total % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return String(buf);
}
