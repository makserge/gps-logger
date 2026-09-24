#include "sync_manager.h"
#include "config_manager.h"
#include "track_store.h"
#include "power_manager.h"
#include "gps_reader.h"
#include "web_server_ui.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

SyncManager syncManager;

// Sync state file tracks which session paths have been successfully uploaded,
// so we don't resend them on every wake cycle.
static bool isPathSynced(const String &path) {
    if (!LittleFS.exists(FS_SYNC_STATE_PATH)) return false;
    File f = LittleFS.open(FS_SYNC_STATE_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    deserializeJson(doc, f);
    f.close();
    for (JsonVariant v : doc["synced"].as<JsonArray>()) {
        if (v.as<String>() == path) return true;
    }
    return false;
}

static void markPathSyncedOnDisk(const String &path) {
    JsonDocument doc;
    if (LittleFS.exists(FS_SYNC_STATE_PATH)) {
        File f = LittleFS.open(FS_SYNC_STATE_PATH, "r");
        if (f) { deserializeJson(doc, f); f.close(); }
    }
    JsonArray arr = doc["synced"].is<JsonArray>() ? doc["synced"].as<JsonArray>() : doc["synced"].to<JsonArray>();
    arr.add(path);
    File f = LittleFS.open(FS_SYNC_STATE_PATH, "w");
    if (f) { serializeJson(doc, f); f.close(); }
}

void SyncManager::begin() {
    // Nothing to initialize since MQTT's removal - kept as the lifecycle
    // hook so setup() stays uniform and future needs have a home.
}

bool SyncManager::isWifiConnected() { return WiFi.status() == WL_CONNECTED; }

void SyncManager::postStatusToBackend() {
    if (!isWifiConnected()) return;
    if (_uploadDisabled) return; // backend already declared unreachable this cycle

    auto &cfg = configManager.get();
    if (strlen(cfg.backendHost) == 0) return;

    JsonDocument doc;
    doc["deviceId"] = configManager.deviceId();
    doc["battery"] = powerManager.readBatteryPercent();
    doc["charging"] = powerManager.isCharging();
    doc["fw"] = FW_VERSION;
    doc["ramPointsUsed"] = trackStore.ramPointCount();
    doc["ramPointsCapacity"] = RAM_TRACKPOINT_CAPACITY;
    doc["ramPointsFree"] = RAM_TRACKPOINT_CAPACITY - trackStore.ramPointCount();
    doc["ip"] = WiFi.localIP().toString();

    doc["gpsFix"] = gpsReader.hasFix();
    doc["gpsSatellites"] = gpsReader.satellites();

    time_t now = time(nullptr);
    bool timeSynced = now >= PLAUSIBLE_EPOCH_FLOOR;
    doc["timeSynced"] = timeSynced;
    doc["tzPosix"] = configManager.get().tzPosix;
    if (timeSynced) {
        struct tm tmUtc;
        gmtime_r(&now, &tmUtc);
        char iso[25];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
        doc["boardTime"] = iso; // UTC, ISO 8601

        struct tm tmLocal;
        localtime_r(&now, &tmLocal); // TZ+DST aware, via the POSIX TZ string set by ConfigManager
        char dateBuf[11], timeBuf[9];
        strftime(dateBuf, sizeof(dateBuf), "%d/%m/%Y", &tmLocal);
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmLocal);
        doc["boardDateLocal"] = dateBuf;
        doc["boardTimeLocal"] = timeBuf;
        doc["isDst"] = tmLocal.tm_isdst > 0;
    } else {
        doc["boardTime"] = nullptr;
        doc["boardDateLocal"] = nullptr;
        doc["boardTimeLocal"] = nullptr;
        doc["isDst"] = nullptr;
    }

    String out;
    serializeJson(doc, out);

    HTTPClient http;
    String url = "http://" + String(cfg.backendHost) + ":" + String(cfg.backendPort) +
                 "/api/v1/devices/" + configManager.deviceId() + "/status";
    http.begin(_wifiClient, url);
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    int code = http.POST(out);
    http.end();

    if (code == 200 || code == 201 || code == 204) {
        _statusPostFailLogged = false;
    } else if (!_statusPostFailLogged) {
        // Log the first failure only - with the backend down this runs every
        // sync check and would otherwise flood serial. Uploads share the
        // backend and their give-up counter silences this path too.
        Serial.printf("[Sync] Status post failed (HTTP %d)\n", code);
        _statusPostFailLogged = true;
    }
}

bool SyncManager::uploadSessionFile(const String &date, const String &sessionFile) {
    auto &cfg = configManager.get();
    String path = trackStore.sessionPath(date, sessionFile);
    if (isPathSynced(path)) return true;

    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t fileSize = f.size();

    HTTPClient http;
    String url = "http://" + String(cfg.backendHost) + ":" + String(cfg.backendPort) +
                 "/api/v1/tracks/" + configManager.deviceId() + "/" + date + "/" + sessionFile;

    http.begin(_wifiClient, url);
    http.addHeader("Content-Type", "application/gpx+xml");
    http.setTimeout(15000);

    int code = http.sendRequest("PUT", &f, fileSize);
    f.close();

    bool ok = (code == 200 || code == 201 || code == 204);
    if (ok) {
        markPathSyncedOnDisk(path);
        invalidatePendingCache();
        _uploadFailCount = 0;
        Serial.printf("[Sync] Uploaded %s (HTTP %d)\n", path.c_str(), code);
    } else {
        _uploadFailCount++;
        Serial.printf("[Sync] Upload failed %s (HTTP %d, attempt %u/%u)\n",
                      path.c_str(), code, _uploadFailCount, SYNC_MAX_UPLOAD_FAILURES);
        if (_uploadFailCount >= SYNC_MAX_UPLOAD_FAILURES) {
            _uploadDisabled = true;
            Serial.println("[Sync] Backend unreachable - uploads disabled until next boot/wake (files stay pending)");
        }
    }
    http.end();
    return ok;
}

void SyncManager::clearSyncState() {
    if (LittleFS.exists(FS_SYNC_STATE_PATH)) {
        LittleFS.remove(FS_SYNC_STATE_PATH);
    }
    invalidatePendingCache();
    Serial.println("[Sync] Sync ledger cleared - all sessions on flash will re-upload");
}

int SyncManager::countPendingSessions() {
    int pending = 0;
    auto dates = trackStore.listSessionDates();
    for (auto &date : dates) {
        for (auto &s : trackStore.listSessionsForDate(date)) {
            if (!isPathSynced(trackStore.sessionPath(date, s))) pending++;
        }
    }
    return pending;
}

int SyncManager::countPendingSessionsCached() {
    unsigned long now = millis();
    if (_pendingCacheMs != 0 && (now - _pendingCacheMs) < 5000 && _pendingCache >= 0) {
        return _pendingCache;
    }
    _pendingCache = countPendingSessions();
    _pendingCacheMs = now;
    return _pendingCache;
}

SyncStatus SyncManager::getStatus() {
    SyncStatus st;
    st.uploadsGaveUp = _uploadDisabled;
    st.uploadFails = _uploadFailCount;
    st.pendingSessions = countPendingSessionsCached();
    st.syncing = _syncing;
    st.syncCurrent = _syncCurrent;
    st.syncTotal = _syncTotal;
    st.lastUploaded = _lastUploaded;
    st.lastAttemptMs = _lastAttemptMs;
    return st;
}

int SyncManager::syncPendingSessions() {
    if (!isWifiConnected()) return 0;
    if (_uploadDisabled) return 0; // gave up this wake cycle - see header

    // Always flush current RAM buffer first so the newest data is included
    trackStore.flushToFlash();

    _lastAttemptMs = millis();
    _syncTotal = countPendingSessions();
    _syncCurrent = 0;
    _syncing = true;

    int syncedCount = 0;
    auto dates = trackStore.listSessionDates();
    for (auto &date : dates) {
        auto sessions = trackStore.listSessionsForDate(date);
        for (auto &s : sessions) {
            bool wasPending = !isPathSynced(trackStore.sessionPath(date, s));
            if (uploadSessionFile(date, s) && wasPending) syncedCount++;
            if (wasPending) _syncCurrent++;
            // Service the web server between files so the UI stays reachable
            // (and its status poll shows live syncCurrent/syncTotal) during a
            // long multi-file pass - each upload blocks for seconds. Between
            // files only, never mid-file, so upload state can't be corrupted.
            webUi.loop();
            if (_uploadDisabled) { // limit hit mid-run - stop hammering the rest
                _syncing = false;
                _lastUploaded = syncedCount;
                return syncedCount;
            }
        }
    }
    _syncing = false;
    _lastUploaded = syncedCount;
    return syncedCount;
}

void SyncManager::ntpSyncIfNeeded() {
    // Charging-only, and once per boot/wake cycle: GPS is the primary time
    // source (see gps_reader.cpp - it syncs the clock from NMEA data
    // whenever it gets a fix, on battery in the field same as always) but
    // frequently can't get a fix at all while charging indoors on a desk.
    // NTP fills that specific gap. Restricting it to charging-only means it
    // never costs extra battery in the field, where GPS already handles
    // this - it only ever runs when power isn't a constraint to begin with.
    if (_ntpRequested) return;
    if (!powerManager.isCharging()) return;

    Serial.println("[NTP] Charging + WiFi connected - starting NTP time sync (supplements GPS)");
    // gmtOffset/daylightOffset both 0: we only want the underlying UTC
    // epoch set correctly here. time_t is always UTC internally regardless
    // of timezone - local/DST-aware display conversion happens separately
    // via localtime_r() using the TZ POSIX string (see ConfigManager).
    // NOTE: configTime() has a side effect of setting its own TZ from the
    // offsets passed to it (effectively "UTC0" here), clobbering whatever
    // TZ ConfigManager already applied - immediately re-assert it after.
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
    configManager.applyTimezone();
    _ntpRequested = true;
    // The ESP32's SNTP client updates the system clock asynchronously in
    // the background from here - no blocking wait needed. Subsequent
    // time(nullptr)/localtime_r() calls anywhere in the firmware will
    // reflect it as soon as the first response arrives. Only the epoch
    // itself updates asynchronously, not the TZ setting - that was already
    // reasserted synchronously above, so no further re-application needed
    // once the actual sync completes in the background.
}

void SyncManager::loop() {
    if (!isWifiConnected()) return;

    ntpSyncIfNeeded();

    unsigned long now = millis();
    bool intervalDue = (now - _lastSyncCheck >= WIFI_SYNC_CHECK_INTERVAL_MS);

    if (_syncRequested || intervalDue) {
        _lastSyncCheck = now;
        _syncRequested = false;
        // Only run a pass when something is actually pending - checking mere
        // file EXISTENCE here (as before) meant a fully-synced device
        // re-walked and re-parsed sync state every 30s forever, uploading
        // nothing. New data invalidates the cache via addPoint->flush paths
        // ending in an upload, and at worst the 5s cache delays a pass by
        // one interval.
        if (countPendingSessionsCached() > 0) {
            syncPendingSessions();
        }
        postStatusToBackend();
    }
}
