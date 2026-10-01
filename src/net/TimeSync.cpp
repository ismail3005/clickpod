#include "TimeSync.h"

#include <SD.h>
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

// Set once by begin(), read-only from then on inside the background task
// -- never touched by more than one task, so no locking needed. The SD
// read that PRODUCES this vector happens before this task ever starts
// (see TimeSync.h's big comment on why).
std::vector<WifiCredential> knownCredentials;

constexpr uint32_t kResyncIntervalMs = 6UL * 60 * 60 * 1000; // 6 hours -- corrects millis() drift
// If a sync attempt is SKIPPED before it even touches the radio (not
// enough internal heap headroom, or Bluetooth currently owns it -- see
// RadioLock.h), retry much sooner than the normal 6h cadence, since both
// are likely transient (heap recovers once boot-time temporaries are
// freed; BT gets turned off). A sync that was actually ATTEMPTED and
// failed for a real reason (no open network in range, couldn't join,
// NTP didn't answer) does NOT get this fast retry -- that's an ordinary,
// possibly-permanent condition (no open network anywhere nearby, ever),
// and hammering WiFi scans every 2 minutes forever over it would just
// waste battery for no benefit.
constexpr uint32_t kSkippedRetryDelayMs = 2UL * 60 * 1000; // 2 minutes
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr uint32_t kNtpTimeoutMs = 5000;

enum class SyncResult { kOk, kSkipped, kFailed };

SyncResult tryOnce() {
    // Internal-heap guard FIRST -- a real WiFi init failure here aborted
    // the whole device in the field (esp_timer_create -> ESP_ERR_NO_MEM,
    // right after the boot-time library scan), so this has to be checked
    // before anything else, including acquiring the radio lock below.
    if (!radioHeapOk("TimeSync")) return SyncResult::kSkipped;

    // See RadioLock.h -- skip this cycle entirely if Bluetooth currently
    // owns the radio, rather than risk the WiFi+BT coexistence crash that
    // prompted adding this lock. Just retries sooner (kSkippedRetryDelayMs).
    RadioLock::ScopedLock lock;
    if (!lock.acquired) {
        Serial.println(F("[time] skipping sync -- Bluetooth is active"));
        return SyncResult::kSkipped;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks();
    if (n <= 0) {
        WiFi.scanDelete();
        Serial.println(F("[time] no networks found"));
        return SyncResult::kFailed;
    }

    // Prefer a network matching one of the SD-card credentials (the
    // user's own phone/laptop hotspot -- deliberately chosen, likely more
    // reliably present than a random open network) over an open network.
    String chosenSsid;
    String chosenPassword; // empty means open network, no password needed
    for (int i = 0; i < n && chosenSsid.length() == 0; i++) {
        String ssid = WiFi.SSID(i);
        for (const WifiCredential &cred : knownCredentials) {
            if (ssid == cred.ssid) {
                chosenSsid = ssid;
                chosenPassword = cred.password;
                break;
            }
        }
    }
    if (chosenSsid.length() == 0) {
        for (int i = 0; i < n; i++) {
            if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
                chosenSsid = WiFi.SSID(i);
                break;
            }
        }
    }
    WiFi.scanDelete();

    if (chosenSsid.length() == 0) {
        Serial.println(F("[time] no known or open network nearby to grab time from"));
        return SyncResult::kFailed;
    }

    Serial.printf("[time] joining \"%s\" for NTP (%s)...\n", chosenSsid.c_str(),
                  chosenPassword.length() > 0 ? "known network" : "open network");
    if (chosenPassword.length() > 0) {
        WiFi.begin(chosenSsid.c_str(), chosenPassword.c_str());
    } else {
        WiFi.begin(chosenSsid.c_str());
    }
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < kConnectTimeoutMs) {
        delay(200);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[time] couldn't join in time, will retry later"));
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return SyncResult::kFailed;
    }

    configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // fetched as UTC; offset applied at display time
    struct tm timeinfo;
    bool ok = getLocalTime(&timeinfo, kNtpTimeoutMs);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF); // radio off between syncs -- no reason to keep it on

    if (!ok) {
        Serial.println(F("[time] NTP fetch failed"));
        return SyncResult::kFailed;
    }

    syncedEpochUtc = time(nullptr);
    syncMillisAt = millis();
    synced = true;
    Serial.println(F("[time] synced"));
    return SyncResult::kOk;
}

// Retries fast (kSkippedRetryDelayMs) only while attempts are being
// skipped pre-radio; stops retrying fast the moment an attempt actually
// runs, whether it then succeeded or failed for a real reason -- that
// case waits for the next full kResyncIntervalMs cycle instead.
void tryUntilAttempted() {
    while (tryOnce() == SyncResult::kSkipped) {
        vTaskDelay(pdMS_TO_TICKS(kSkippedRetryDelayMs));
    }
}

void taskFn(void *) {
    tryUntilAttempted();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kResyncIntervalMs));
        tryUntilAttempted();
    }
}

} // namespace

std::vector<WifiCredential> loadCredentialsFromSd(const char *path) {
    std::vector<WifiCredential> out;
    if (!SD.exists(path)) return out; // optional file -- no card, or user hasn't made one yet

    File f = SD.open(path);
    if (!f) return out;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;

        int comma = line.indexOf(',');
        if (comma < 0) {
            Serial.printf("[time] skipping malformed line in %s (no comma): \"%s\"\n", path, line.c_str());
            continue;
        }

        WifiCredential cred;
        cred.ssid = line.substring(0, comma);
        cred.ssid.trim();
        cred.password = line.substring(comma + 1);
        cred.password.trim();
        if (cred.ssid.length() == 0) continue;

        out.push_back(cred);
    }
    f.close();

    Serial.printf("[time] loaded %u WiFi credential(s) from %s\n", (unsigned)out.size(), path);
    return out;
}

void begin(std::vector<WifiCredential> credentials) {
    knownCredentials = std::move(credentials);
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
