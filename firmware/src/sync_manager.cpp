#include "sync_manager.h"
#include "debug_log.h"
#include "config_manager.h"
#include "track_store.h"
#include "power_manager.h"
#include "gps_reader.h"
#include "web_server_ui.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <esp_sntp.h>
#include "lifecycle.h"
#include <ArduinoJson.h>
#include <time.h>
#include <algorithm>

SyncManager syncManager;

static const char *LEDGER_TMP_PATH = "/sync_state.tmp";

// Size of a file on flash, 0 if missing. A session file is created (0
// bytes) the moment it's opened for writing and only gets content when that
// write completes - a reset or power loss in between leaves an empty file
// behind. Such files have nothing to upload and must never reach
// HTTPClient: with size 0 its stream upload drops Content-Length and loops
// until available() returns -1, which a File at EOF never does - the loop
// then hangs forever (web UI dead, no sleep) on every boot.
static uint32_t fileSize(const String &path) {
    if (!LittleFS.exists(path)) return 0;
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    uint32_t sz = (uint32_t)f.size();
    f.close();
    return sz;
}

// -----------------------------------------------------------------------
// Sync ledger - see LedgerEntry in sync_manager.h
// On flash: {"v":2,"synced":["host:port|/tracks/D/F.gpx|12345", ...]}.
// Older formats (no size) are ignored, i.e. those files upload once more -
// harmless, uploads are idempotent PUTs.
// -----------------------------------------------------------------------
void SyncManager::loadLedger() {
    if (_ledgerLoaded) return;
    _ledgerLoaded = true;
    _ledger.clear();
    if (!LittleFS.exists(FS_SYNC_STATE_PATH)) return;
    File f = LittleFS.open(FS_SYNC_STATE_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Log.printf("[Sync] Ledger unreadable (%s) - starting fresh, files will re-upload\n", err.c_str());
        return;
    }
    bool pruned = false;
    for (JsonVariant v : doc["synced"].as<JsonArray>()) {
        String e = v.as<String>();
        int a = e.indexOf('|');
        int b = e.lastIndexOf('|');
        if (a <= 0 || b <= a) { pruned = true; continue; } // legacy/unsized entry
        LedgerEntry le{e.substring(0, a), e.substring(a + 1, b), (uint32_t)e.substring(b + 1).toInt()};
        // Entries of files no longer on flash (deleted, reclaimed) would
        // only ever grow the ledger - drop them here.
        if (!LittleFS.exists(le.path)) { pruned = true; continue; }
        _ledger.push_back(le);
    }
    if (pruned) saveLedger();
}

// Atomic: written to a temp file and renamed over the old one (LittleFS
// rename replaces atomically), so a reset mid-write can't leave a
// truncated ledger behind.
bool SyncManager::saveLedger() {
    JsonDocument doc;
    doc["v"] = 2;
    JsonArray arr = doc["synced"].to<JsonArray>();
    for (auto &e : _ledger) arr.add(e.key + "|" + e.path + "|" + String(e.size));
    File f = LittleFS.open(LEDGER_TMP_PATH, "w");
    if (!f) return false;
    size_t expected = measureJson(doc);
    size_t written = serializeJson(doc, f);
    f.close();
    if (written != expected || fileSize(LEDGER_TMP_PATH) != expected) {
        LittleFS.remove(LEDGER_TMP_PATH);
        Log.println("[Sync] Could not save the sync ledger (flash full?) - kept in RAM");
        return false;
    }
    return LittleFS.rename(LEDGER_TMP_PATH, FS_SYNC_STATE_PATH);
}

bool SyncManager::isSynced(const String &key, const String &path, uint32_t size) {
    loadLedger();
    for (auto &e : _ledger) {
        if (e.size == size && e.path == path && e.key == key) return true;
    }
    return false;
}

bool SyncManager::isSyncedAnywhere(const String &path, uint32_t size) {
    loadLedger();
    for (auto &e : _ledger) {
        if (e.size == size && e.path == path) return true;
    }
    return false;
}

void SyncManager::markSynced(const String &key, const String &path, uint32_t size) {
    loadLedger();
    for (auto &e : _ledger) {
        if (e.path == path && e.key == key) {
            e.size = size;
            saveLedger();
            return;
        }
    }
    _ledger.push_back(LedgerEntry{key, path, size});
    saveLedger();
}

void SyncManager::forgetPath(const String &path) {
    loadLedger();
    size_t before = _ledger.size();
    _ledger.erase(std::remove_if(_ledger.begin(), _ledger.end(),
                                 [&](const LedgerEntry &e) { return e.path == path; }),
                  _ledger.end());
    if (_ledger.size() != before) saveLedger();
    _rejected.erase(std::remove_if(_rejected.begin(), _rejected.end(),
                                   [&](const Rejected &r) { return r.path == path; }),
                    _rejected.end());
    _fileFails.erase(std::remove_if(_fileFails.begin(), _fileFails.end(),
                                    [&](const FileFails &ff) { return ff.path == path; }),
                     _fileFails.end());
    invalidatePendingCache();
}

void SyncManager::forgetDate(const String &date) {
    loadLedger();
    String prefix = String(FS_SESSIONS_DIR) + "/" + date + "/";
    size_t before = _ledger.size();
    _ledger.erase(std::remove_if(_ledger.begin(), _ledger.end(),
                                 [&](const LedgerEntry &e) { return e.path.startsWith(prefix); }),
                  _ledger.end());
    if (_ledger.size() != before) saveLedger();
    _rejected.erase(std::remove_if(_rejected.begin(), _rejected.end(),
                                   [&](const Rejected &r) { return r.path.startsWith(prefix); }),
                    _rejected.end());
    _fileFails.erase(std::remove_if(_fileFails.begin(), _fileFails.end(),
                                    [&](const FileFails &ff) { return ff.path.startsWith(prefix); }),
                     _fileFails.end());
    invalidatePendingCache();
}

bool SyncManager::isRejected(const String &key, const String &path, uint32_t size) {
    unsigned long now = millis();
    for (auto &r : _rejected) {
        if (r.size == size && r.path == path && r.key == key) {
            return now - r.atMs < SYNC_REJECT_RETRY_MS;
        }
    }
    return false;
}

// One entry per (backend, file): a re-rejected file (new size) replaces its
// old entry instead of piling up stale ones in the "skipped" count.
void SyncManager::addRejected(const String &key, const String &path, uint32_t size) {
    for (auto &r : _rejected) {
        if (r.path == path && r.key == key) {
            r.size = size;
            r.atMs = millis();
            return;
        }
    }
    _rejected.push_back(Rejected{key, path, size, millis()});
}

void SyncManager::noteFileFailure(const String &key, const String &path, uint32_t size) {
    for (auto &ff : _fileFails) {
        if (ff.path == path && ff.key == key) {
            if (++ff.count >= SYNC_FILE_MAX_FAILURES) {
                addRejected(key, path, size);
                Log.printf("[Sync] %s failed %u times - parked until it changes or Sync now\n",
                           path.c_str(), (unsigned)ff.count);
                ff.count = 0;
            }
            return;
        }
    }
    _fileFails.push_back(FileFails{key, path, 1});
}

void SyncManager::clearFileFailures(const String &key, const String &path) {
    _fileFails.erase(std::remove_if(_fileFails.begin(), _fileFails.end(),
                                    [&](const FileFails &ff) { return ff.path == path && ff.key == key; }),
                     _fileFails.end());
}

// LittleFS allocates whole blocks: a deleted file frees its size rounded up.
static size_t blockRounded(size_t bytes) {
    const size_t block = 4096;
    return ((bytes + block - 1) / block) * block;
}

void SyncManager::reclaimSpace(size_t extraNeeded) {
    // Last resort only: free just what the imminent flush needs - the
    // device keeps every track it has room for - but so that the reserve
    // is still free AFTER the flush. (Measured before it, the flush itself
    // used up the reserve: 16 KB free mid-drive on 2026-10-07, and the
    // debug log, which pauses below its own 64 KB floor, lost 15 minutes.)
    size_t target = extraNeeded + (size_t)SYNC_FS_MIN_FREE_BYTES;
    bool mayDeleteSynced = configManager.get().reclaimSynced;
    // Only when usage changed noticeably, or every 10 min - this runs on
    // every sync check and before every flush.
    static size_t lastLoggedUsed = 0;
    static unsigned long lastLoggedMs = 0;
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes(); // a full filesystem walk - computed once here
    size_t delta = used > lastLoggedUsed ? used - lastLoggedUsed : lastLoggedUsed - used;
    if (lastLoggedMs == 0 || delta >= 16 * 1024 || millis() - lastLoggedMs >= 600000UL) {
        LOGD("[Sync] Flash %u/%u KB used (needs %u KB free)\n",
             (unsigned)(used / 1024), (unsigned)(total / 1024),
             (unsigned)(target / 1024));
        lastLoggedUsed = used;
        lastLoggedMs = millis();
    }

    // The live session and its date folder are never touched: the folder
    // is created at session start but stays EMPTY until the first flush
    // (up to 15 min later) - removing it as "empty" made every flush of
    // that session fail, losing whole trips.
    String current = trackStore.currentSessionPath();
    String currentDate = trackStore.currentSessionDate();
    auto dates = trackStore.listSessionDates();
    std::sort(dates.begin(), dates.end()); // oldest first

    // Empty session files are garbage from an interrupted write.
    for (auto &date : dates) {
        for (auto &s : trackStore.listSessionsForDate(date)) {
            String path = trackStore.sessionPath(date, s);
            if (path != current && fileSize(path) == 0) {
                if (trackStore.deleteSession(date, s)) {
                    forgetPath(path);
                    Log.printf("[Sync] Removed empty session file %s\n", path.c_str());
                }
            }
        }
    }

    // Then, while free space is below the reserve, delete the OLDEST
    // sessions whose current version some backend has. Free space is
    // tracked by subtracting what's deleted (re-walking the filesystem per
    // file is expensive); rounding to blocks makes it slightly pessimistic.
    size_t freeBytes = total > used ? total - used : 0;
    for (auto &date : dates) {
        if (!mayDeleteSynced || freeBytes >= target) break;
        auto sessions = trackStore.listSessionsForDate(date);
        std::sort(sessions.begin(), sessions.end());
        for (auto &s : sessions) {
            if (freeBytes >= target) break;
            String path = trackStore.sessionPath(date, s);
            if (path == current) continue;
            uint32_t sz = fileSize(path);
            if (sz == 0 || !isSyncedAnywhere(path, sz)) continue;
            if (trackStore.deleteSession(date, s)) {
                forgetPath(path);
                freeBytes += blockRounded(sz);
                Log.printf("[Sync] Low flash (%u KB free) - removed synced %s\n",
                           (unsigned)(freeBytes / 1024), path.c_str());
                invalidatePendingCache();
            }
        }
    }
    for (auto &date : dates) {
        if (date == currentDate) continue;
        if (trackStore.listSessionsForDate(date).empty()) trackStore.deleteDate(date);
    }
    // Still short with nothing synced left to delete: stale debug logs go
    // before any track data does - whether recording is on or off.
    if (freeBytes < target) {
        size_t freed = Log.releaseSpace(freeBytes < DEBUG_LOG_MIN_FREE_BYTES);
        if (freed) Log.printf("[Sync] Low flash - removed %u KB of debug log\n", (unsigned)(freed / 1024));
    }
}

StorageStatus SyncManager::storageStatus() {
    static StorageStatus cached{};
    static unsigned long cachedAtMs = 0;
    unsigned long now = millis();
    if (cachedAtMs != 0 && now - cachedAtMs < 30000UL) return cached;

    StorageStatus st{};
    st.totalBytes = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    st.freeBytes = st.totalBytes > used ? st.totalBytes - used : 0;
    bool mayDelete = configManager.get().reclaimSynced;
    String current = trackStore.currentSessionPath();
    for (auto &date : trackStore.listSessionDates()) {
        for (auto &s : trackStore.listSessionsForDate(date)) {
            String path = trackStore.sessionPath(date, s);
            uint32_t sz = fileSize(path);
            if (sz == 0) continue;
            if (isSyncedAnywhere(path, sz)) {
                if (mayDelete && path != current) st.reclaimableBytes += blockRounded(sz);
            } else {
                st.unsyncedBytes += sz;
                st.unsyncedFiles++;
            }
        }
    }
    uint32_t effectiveFree = st.freeBytes + st.reclaimableBytes;
    st.level = effectiveFree < STORAGE_CRITICAL_BYTES ? 2 : (effectiveFree < STORAGE_WARN_BYTES ? 1 : 0);
    if (st.level != cached.level || cachedAtMs == 0) {
        if (st.level) {
            Log.printf("[Storage] %s: %lu KB free + %lu KB reclaimable, %lu KB in %d track(s) not uploaded\n",
                       st.level == 2 ? "FULL" : "Almost full", (unsigned long)(st.freeBytes / 1024),
                       (unsigned long)(st.reclaimableBytes / 1024), (unsigned long)(st.unsyncedBytes / 1024),
                       st.unsyncedFiles);
        } else if (cachedAtMs != 0) {
            Log.println("[Storage] Space OK again");
        }
    }
    cached = st;
    cachedAtMs = now;
    return st;
}

void SyncManager::begin() {
    loadLedger();
}

bool SyncManager::isWifiConnected() { return WiFi.status() == WL_CONNECTED; }

const char *SyncManager::activeHost() {
    auto &cfg = configManager.get();
    return (_activeBackend == 1) ? cfg.backendHost2 : cfg.backendHost;
}

uint16_t SyncManager::activePort() {
    auto &cfg = configManager.get();
    return (_activeBackend == 1) ? cfg.backendPort2 : cfg.backendPort;
}

String SyncManager::activeBackendKey() {
    return String(activeHost()) + ":" + String(activePort());
}

// GET /healthz with tight timeouts - just "is a backend alive at this
// address". Deliberately not the upload endpoint: cheap, side-effect-free.
static bool probeBackend(WiFiClient &client, const char *host, uint16_t port) {
    if (strlen(host) == 0) return false;
    HTTPClient http;
    String url = "http://" + String(host) + ":" + String(port) + "/healthz";
    http.begin(client, url);
    http.setConnectTimeout(SYNC_PROBE_CONNECT_MS);
    http.setTimeout(3000);
    int code = http.GET();
    http.end();
    LOGD("[Sync] Probe %s:%u -> %d\n", host, port, code);
    return code == 200;
}

bool SyncManager::resolveBackend() {
    if (_activeBackend >= 0) return true;
    if (!isWifiConnected()) return false;
    auto &cfg = configManager.get();
    if (probeBackend(_wifiClient, cfg.backendHost, cfg.backendPort)) {
        _activeBackend = 0;
    } else if (probeBackend(_wifiClient, cfg.backendHost2, cfg.backendPort2)) {
        _activeBackend = 1;
    } else {
        return false;
    }
    Log.printf("[Sync] Active backend: %s\n", activeBackendKey().c_str());
    invalidatePendingCache(); // pending count is per-backend
    return true;
}

void SyncManager::noteFailure(const char *what, unsigned long pauseMs) {
    _uploadFailCount++;
    if (_uploadFailCount >= SYNC_MAX_UPLOAD_FAILURES && !_uploadDisabled) {
        _uploadDisabled = true;
        _disabledAtMs = millis();
        _pauseMs = pauseMs;
        Log.printf("[Sync] %s - sync paused for %lu min (files stay pending)\n",
                   what, (unsigned long)(pauseMs / 60000UL));
    }
}

void SyncManager::postStatusToBackend() {
    if (!isWifiConnected()) return;
    if (_uploadDisabled) return; // backend already declared unreachable this cycle
    if (!resolveBackend()) return;

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
    String url = "http://" + String(activeHost()) + ":" + String(activePort()) +
                 "/api/v1/devices/" + configManager.deviceId() + "/status";
    http.begin(_wifiClient, url);
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    int code = http.POST(out);
    http.end();

    LOGD("[Sync] Status post -> HTTP %d\n", code);
    if (code == 200 || code == 201 || code == 204) {
        _statusPostFailLogged = false;
    } else if (!_statusPostFailLogged) {
        // Log the first failure only - with the backend down this runs every
        // sync check and would otherwise flood serial. Uploads share the
        // backend and their give-up counter silences this path too.
        Log.printf("[Sync] Status post failed (HTTP %d)\n", code);
        _statusPostFailLogged = true;
    }
}

SyncManager::UploadResult SyncManager::uploadSessionFile(const String &date, const String &sessionFile, uint32_t size) {
    String path = trackStore.sessionPath(date, sessionFile);
    if (size == 0) return UploadResult::Skipped; // see fileSize()

    File f = LittleFS.open(path, "r");
    if (!f) return UploadResult::Skipped;
    // The size read before opening is the version this upload sends and
    // records - nothing can append while the (single-task) upload runs.
    size = (uint32_t)f.size();
    if (size == 0) {
        f.close();
        return UploadResult::Skipped;
    }

    HTTPClient http;
    String url = "http://" + String(activeHost()) + ":" + String(activePort()) +
                 "/api/v1/tracks/" + configManager.deviceId() + "/" + date + "/" + sessionFile;

    http.begin(_wifiClient, url);
    http.addHeader("Content-Type", "application/gpx+xml");
    http.setConnectTimeout(5000);
    http.setTimeout(15000);

    int code = http.sendRequest("PUT", &f, size);
    f.close();
    http.end();

    String key = activeBackendKey();
    if (code == 200 || code == 201 || code == 204) {
        markSynced(key, path, size);
        clearFileFailures(key, path);
        invalidatePendingCache();
        // Deliberately NOT resetting _uploadFailCount here: the growing live
        // session re-uploads successfully every pass, which kept resetting
        // it while another file failed forever. A pass without failures
        // resets it instead (see syncPendingSessions()).
        Log.printf("[Sync] Uploaded %s (%lu bytes, HTTP %d)\n", path.c_str(), (unsigned long)size, code);
        return UploadResult::Ok;
    }
    if (code >= 400 && code < 500 && code != 408 && code != 429) {
        // The backend is reachable and answered - it rejected THIS file
        // (e.g. damaged beyond repair). Not a connectivity failure, so it
        // doesn't count toward the give-up; not retried by this backend
        // until it changes (or Sync now / a backend change / reconnect).
        addRejected(key, path, size);
        invalidatePendingCache();
        Log.printf("[Sync] Backend rejected %s (HTTP %d) - skipped until it changes\n", path.c_str(), code);
        return UploadResult::Rejected;
    }
    Log.printf("[Sync] Upload failed %s (HTTP %d, attempt %u/%u)\n",
               path.c_str(), code, (unsigned)_uploadFailCount + 1, (unsigned)SYNC_MAX_UPLOAD_FAILURES);
    // Connection-type failures (no answer, timeouts, gateway errors while
    // the backend restarts) say nothing about THIS file - they don't count
    // toward parking it; they make the next check re-probe the backends
    // instead (failover to the other one). Only an answering server that
    // keeps failing on this file parks it.
    bool connectionClass = code < 0 || code == 502 || code == 503 || code == 504;
    if (connectionClass) _reprobeAfterPass = true;
    else noteFileFailure(key, path, size);
    noteFailure("Backend unreachable");
    return UploadResult::Failed;
}

int SyncManager::countPendingSessions() {
    // Pending is relative to the ACTIVE backend's ledger namespace; while
    // unresolved this falls back to the primary's key (display-only - the
    // sync pass itself always resolves first, see loop()).
    String key = activeBackendKey();
    int pending = 0;
    for (auto &date : trackStore.listSessionDates()) {
        for (auto &s : trackStore.listSessionsForDate(date)) {
            String path = trackStore.sessionPath(date, s);
            uint32_t sz = fileSize(path);
            if (sz > 0 && !isSynced(key, path, sz) && !isRejected(key, path, sz)) pending++;
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
    st.rejected = (int)_rejected.size();
    return st;
}

int SyncManager::syncPendingSessions() {
    if (!isWifiConnected()) return 0;
    if (_uploadDisabled) return 0; // paused after repeated failures - see noteFailure()
    if (!resolveBackend()) {       // e.g. called from goToSleep() before any check ran
        noteFailure("No backend reachable");
        return 0;
    }

    // Flush first so the newest points are included (the live session is
    // uploaded too - and again whenever it has grown, see LedgerEntry).
    // The flush runs the space hook (reclaimSpace) itself.
    trackStore.flushToFlash();

    _lastAttemptMs = millis();
    _syncTotal = countPendingSessions();
    _syncCurrent = 0;
    _syncing = true;

    String key = activeBackendKey();
    int syncedCount = 0;
    bool passHadFailure = false;
    for (auto &date : trackStore.listSessionDates()) {
        for (auto &s : trackStore.listSessionsForDate(date)) {
            String path = trackStore.sessionPath(date, s);
            uint32_t sz = fileSize(path);
            if (sz == 0 || isSynced(key, path, sz) || isRejected(key, path, sz)) continue;
            UploadResult res = uploadSessionFile(date, s, sz);
            if (res == UploadResult::Ok) syncedCount++;
            if (res == UploadResult::Failed) passHadFailure = true;
            _syncCurrent++;
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
    if (!passHadFailure) _uploadFailCount = 0; // a clean pass ends a failure streak
    if (_reprobeAfterPass) {
        _reprobeAfterPass = false;
        _activeBackend = -1; // next check probes both backends again
    }
    reclaimSpace(); // files uploaded in this pass are now reclaimable
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

    Log.println("[NTP] Charging + WiFi connected - starting NTP time sync (supplements GPS)");
    // gmtOffset/daylightOffset both 0: we only want the underlying UTC
    // epoch set correctly here. time_t is always UTC internally regardless
    // of timezone - local/DST-aware display conversion happens separately
    // via localtime_r() using the TZ POSIX string (see ConfigManager).
    // NOTE: configTime() has a side effect of setting its own TZ from the
    // offsets passed to it (effectively "UTC0" here), clobbering whatever
    // TZ ConfigManager already applied - immediately re-assert it after.
    // Each NTP correction is a clock sync like a GPS fix - the reference
    // for the next drift measurement (see noteClockSynced()).
    sntp_set_time_sync_notification_cb([](struct timeval *) { noteClockSynced(); });
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
    unsigned long now = millis();

    // "Sync now" pressed during a pass: apply its reset now that the pass
    // is over.
    if (_manualSyncPending && !_syncing) {
        _manualSyncPending = false;
        resetBackoff();
        _syncRequested = true;
    }

    // A give-up is a pause, not the end: on a charger the device never
    // sleeps or restarts, so "until next wake" meant a backend that was
    // down for a few minutes (HA update) stopped all syncing for days.
    if (_uploadDisabled && now - _disabledAtMs >= _pauseMs) {
        Log.println("[Sync] Retrying after pause");
        resetBackoff();
        _syncRequested = true;
    }

    if (!isWifiConnected()) return;

    ntpSyncIfNeeded();

    bool intervalDue = (now - _lastSyncCheck >= WIFI_SYNC_CHECK_INTERVAL_MS);
    if (!(_syncRequested || intervalDue)) return;
    _lastSyncCheck = now;
    _syncRequested = false;

    if (_uploadDisabled) return;
    // Resolve which backend is reachable BEFORE consulting the pending
    // count: the count is namespaced per backend.
    if (!resolveBackend()) {
        // Re-probe sooner than the regular interval: the first minutes
        // of a window are when the pending upload should happen.
        _lastSyncCheck = now - (WIFI_SYNC_CHECK_INTERVAL_MS - SYNC_PROBE_RETRY_MS);
        _lastCheckIdle = false;
        noteFailure("No backend reachable", SYNC_NO_BACKEND_RETRY_MS);
        return;
    }
    int pending = countPendingSessionsCached();
    _lastCheckIdle = (pending == 0);
    LOGD("[Sync] Check: %d pending for %s\n", pending, activeBackendKey().c_str());
    if (pending > 0) {
        int n = syncPendingSessions();
        LOGD("[Sync] Pass done: %d uploaded, %u consecutive failures%s\n",
             n, (unsigned)_uploadFailCount, _uploadDisabled ? ", paused" : "");
    } else {
        reclaimSpace(); // flash can fill up while everything is synced
    }
    postStatusToBackend();
}
