#include "TimeSync.h"

#include <WiFi.h>
#include <atomic>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../state/AppState.h"
#include "RadioLock.h"

namespace TimeSync {
namespace {

std::atomic<bool> synced{false};
std::atomic<time_t> syncedEpochUtc{0}; // written by the background task, read from the main/UI task
std::atomic<uint32_t> syncMillisAt{0};

constexpr uint32_t kResyncIntervalMs = 6UL * 60 * 60 * 1000; // 6 hours -- corrects millis() drift
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kNtpTimeoutMs = 5000;

bool tryOnce() {
    // See RadioLock.h -- skip this cycle entirely if Bluetooth currently
    // owns the radio, rather than risk the WiFi+BT coexistence crash that
    // prompted adding this lock. Just retries next interval.
    RadioLock::ScopedLock lock;
    if (!lock.acquired) {
        Serial.println(F("[time] skipping sync -- Bluetooth is active"));
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks();
    if (n <= 0) {
        WiFi.scanDelete();
        Serial.println(F("[time] no networks found"));
        return false;
    }

    String openSsid;
    for (int i = 0; i < n; i++) {
        if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
            openSsid = WiFi.SSID(i);
            break;
        }
    }
    WiFi.scanDelete();

    if (openSsid.length() == 0) {
        Serial.println(F("[time] no open network nearby to grab time from"));
        return false;
    }

    Serial.printf("[time] joining open network \"%s\" for NTP...\n", openSsid.c_str());
    WiFi.begin(openSsid.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < kConnectTimeoutMs) {
        delay(200);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[time] couldn't join in time, will retry later"));
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // fetched as UTC; offset applied at display time
    struct tm timeinfo;
    bool ok = getLocalTime(&timeinfo, kNtpTimeoutMs);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); // radio off between syncs -- no reason to keep it on

    if (!ok) {
        Serial.println(F("[time] NTP fetch failed"));
        return false;
    }

    syncedEpochUtc = time(nullptr);
    syncMillisAt = millis();
    synced = true;
    Serial.println(F("[time] synced"));
    return true;
}

void taskFn(void *) {
    tryOnce();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kResyncIntervalMs));
        tryOnce();
    }
}

} // namespace

void begin() {
    xTaskCreatePinnedToCore(taskFn, "timesync", 8192, nullptr, 1, nullptr, 0);
}

bool isSynced() { return synced; }

String currentTimeString() {
    if (!synced) return "--:--";
    time_t now = syncedEpochUtc + (time_t)((millis() - syncMillisAt) / 1000);
    now += (time_t)state.utcOffsetHours * 3600;
    struct tm *t = gmtime(&now); // already offset above; gmtime() just splits it out, no further TZ shift
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    return String(buf);
}

} // namespace TimeSync
