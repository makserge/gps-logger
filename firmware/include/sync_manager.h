#pragma once
#include "config.h"
#include <WiFiClient.h>
#include <vector>

// Snapshot of the sync machinery's state, for display in the device web UI
// (see WebUi::handleApiStatus). All counts refer to the current wake cycle
// except pendingSessions, which reflects what's actually on flash.
struct SyncStatus {
    bool uploadsGaveUp;     // SYNC_MAX_UPLOAD_FAILURES in a row - paused, retried after SYNC_GIVEUP_RETRY_MS
    uint8_t uploadFails;    // consecutive upload failures so far
    int pendingSessions;    // session files on flash whose current version the active backend lacks
    bool syncing;           // an upload pass is running right now
    int syncCurrent;        // files attempted so far in the running/last pass
    int syncTotal;          // pending files at the start of the running/last pass
    int lastUploaded;       // files successfully uploaded in the last pass
    int rejected;           // files the backend refused / that keep failing - retried on Sync now or when they change
    unsigned long lastAttemptMs; // millis() of the last pass start, 0 = none yet
};

// How close the track storage is to full. "Effective" free space counts
// the uploaded tracks that reclaimSpace() may delete (only when Settings ->
// Storage allows it) - not-uploaded tracks are never deleted, so once they
// fill the flash, recording stops: that is what the warning is about.
struct StorageStatus {
    uint32_t totalBytes;
    uint32_t freeBytes;
    uint32_t reclaimableBytes; // uploaded tracks deletable if space is needed
    uint32_t unsyncedBytes;    // tracks no backend has yet (never deleted)
    int unsyncedFiles;
    uint8_t level;             // 0 ok, 1 almost full (STORAGE_WARN_BYTES), 2 critical
};

// All backend communication is plain HTTP against the backend's REST API:
// GPX uploads plus a periodic device-status POST (battery/GPS/etc. shown in
// the backend's web UI). MQTT was deliberately REMOVED: its status publish
// is fully replaced by the HTTP POST over the connection that must exist
// anyway, and its only other use ("sync now" command) could never reach a
// sleeping device and was redundant while awake (automatic sync already
// runs on wake, every 30s, and before sleep) - all while costing a broker
// service, a credential chain, and up to ~50s of blocking connect retries
// per wake cycle when the broker was down.
class SyncManager {
public:
    void begin();
    void loop();  // call every iteration; handles periodic sync + status posts

    bool isWifiConnected();

    SyncStatus getStatus();

    // Clears the give-up state and the failure counters and forgets the
    // resolved backend. Called when the backend config changes, when the
    // WiFi (re)connects (possibly to the other site's network), by "Sync
    // now", and automatically SYNC_GIVEUP_RETRY_MS after a give-up.
    void resetBackoff() {
        _uploadFailCount = 0;
        _uploadDisabled = false;
        _activeBackend = -1; // re-resolve
        _rejected.clear();   // give refused/failing files another chance
        invalidatePendingCache();
    }

    // Push all unsynced sessions to backend via REST. Returns count synced.
    int syncPendingSessions();

    // Automatic sync trigger (boot/wake): next loop() runs a check.
    void requestImmediateSync() { _syncRequested = true; }
    // The last check reached a backend and found nothing pending.
    bool lastCheckIdle() const { return _lastCheckIdle; }
    // The web UI's "Sync now": also lifts a give-up and re-probes backends.
    // During a running pass the reset is deferred until the pass ends
    // (resetting the resolved backend mid-pass would send the rest of it to
    // the primary) - see the top of loop().
    void requestManualSync() {
        if (_syncing) _manualSyncPending = true; else resetBackoff();
        _syncRequested = true;
    }
    // WiFi (re)connected: the reachable backend may differ now.
    void onNetworkConnected() { resetBackoff(); _syncRequested = true; }

    // Pending count, cached for a few seconds (status polls every 2-10 s);
    // anything that changes it invalidates explicitly.
    int countPendingSessionsCached();
    void invalidatePendingCache() { _pendingCacheMs = 0; }

    // Probes the configured backend(s) in order (GET /healthz, short
    // timeout) and locks onto the first one that answers until the next
    // resetBackoff(). Returns false when none is reachable.
    bool resolveBackend();
    // "host:port" of the resolved backend, or of the primary as a display
    // fallback while unresolved. Also the sync-ledger namespace, so each
    // backend independently tracks what it has received.
    String activeBackendKey();
    bool isBackendResolved() const { return _activeBackend >= 0; }

    // Local flash housekeeping, independent of WiFi/backends (the ledger
    // is on flash): removes empty session files and - while free space is
    // below what the next flush needs (SYNC_FS_MIN_FREE_BYTES margin), and
    // only if Settings -> Storage allows it - the oldest sessions whose CURRENT
    // version is stored on some backend. Never touches the live session or
    // its date folder, never deletes unsynced data. Runs before every
    // track flush (see TrackStore's space hook) and on every sync check.
    // extraNeeded: bytes an imminent track flush needs, if more than the reserve.
    void reclaimSpace(size_t extraNeeded = 0);

    // See StorageStatus. Walks all session files - cached for 30 s.
    StorageStatus storageStatus();

    // Drop ledger entries of a deleted session / date (web UI delete).
    void forgetPath(const String &path);
    void forgetDate(const String &date);

    // True while an upload pass is running. The web server is serviced
    // BETWEEN file uploads during a pass (for live progress + reachability),
    // which makes its handlers reentrant into sync state - mutating handlers
    // (delete, OTA, backend change, WiFi reset) check this and refuse with
    // 503 rather than yanking files/state out from under the running pass.
    bool isSyncing() const { return _syncing; }

private:
    WiFiClient _wifiClient;
    unsigned long _lastSyncCheck = 0;
    bool _lastCheckIdle = false;
    bool _syncRequested = false;
    // Consecutive REST failures (probe or upload): each blocks the loop for
    // the HTTP timeouts, so after SYNC_MAX_UPLOAD_FAILURES in a row sync
    // pauses; it resumes after SYNC_GIVEUP_RETRY_MS, on "Sync now", on a
    // WiFi reconnect, or at the next wake. Nothing is lost meanwhile.
    uint8_t _uploadFailCount = 0;
    bool _uploadDisabled = false;
    unsigned long _disabledAtMs = 0;
    unsigned long _pauseMs = SYNC_GIVEUP_RETRY_MS; // length of the current give-up pause
    void noteFailure(const char *what, unsigned long pauseMs = SYNC_GIVEUP_RETRY_MS);
    // Progress bookkeeping for the web UI (see SyncStatus/getStatus())
    bool _syncing = false;
    int _syncCurrent = 0;
    int _syncTotal = 0;
    int _lastUploaded = 0;
    unsigned long _lastAttemptMs = 0;

    int countPendingSessions(); // uncached full walk - see countPendingSessionsCached()
    int _pendingCache = -1;
    unsigned long _pendingCacheMs = 0; // 0 = invalid

    // Resolved backend index: -1 = not resolved, 0 = primary, 1 = secondary.
    int _activeBackend = -1;
    const char *activeHost();
    uint16_t activePort();

    // --- Sync ledger (FS_SYNC_STATE_PATH), cached in RAM ---
    // One entry per (backend, session file) with the file SIZE that backend
    // has: a session keeps growing after an upload (the live trip is
    // uploaded whenever a WiFi window allows), so "synced" means "this
    // backend has the file as it is NOW". Keyed by path alone, a trip was
    // marked done after its first minutes and the rest never uploaded.
    struct LedgerEntry {
        String key;   // backend "host:port"
        String path;  // /tracks/<date>/<file>.gpx
        uint32_t size;
    };
    std::vector<LedgerEntry> _ledger;
    bool _ledgerLoaded = false;
    void loadLedger();
    bool saveLedger();
    bool isSynced(const String &key, const String &path, uint32_t size);
    bool isSyncedAnywhere(const String &path, uint32_t size);
    void markSynced(const String &key, const String &path, uint32_t size);

    // Files the backend rejected (HTTP 4xx) this boot, with the size they
    // had: not retried (and not counted as pending) until they change -
    // otherwise one bad file kept a full sync pass (and a track flush)
    // running every 30 s forever.
    // Per backend: another backend may well accept the file. Transient
    // answers (408/429) count as failures instead, and a file that fails
    // SYNC_FILE_MAX_FAILURES times in a row is parked here too, so one
    // broken file can't keep the pass (and the re-upload of the growing
    // live session, whose success reset the failure count) spinning.
    // atMs: entries expire after SYNC_REJECT_RETRY_MS, so nothing is
    // skipped forever while the device stays connected on a charger.
    struct Rejected { String key; String path; uint32_t size; unsigned long atMs; };
    void addRejected(const String &key, const String &path, uint32_t size);
    std::vector<Rejected> _rejected;
    bool isRejected(const String &key, const String &path, uint32_t size);
    struct FileFails { String key; String path; uint8_t count; };
    std::vector<FileFails> _fileFails;
    bool _manualSyncPending = false; // Sync now pressed during a pass
    bool _reprobeAfterPass = false;  // a pass saw connection failures - re-resolve the backend
    void noteFileFailure(const String &key, const String &path, uint32_t size);
    void clearFileFailures(const String &key, const String &path);

    enum class UploadResult { Ok, Rejected, Failed, Skipped };
    UploadResult uploadSessionFile(const String &date, const String &sessionFile, uint32_t size);

    // Periodic device-status POST to the backend (replaces the old MQTT
    // status publish): battery, charging, GPS, RAM buffer, board time.
    // Same backoff gate as uploads (same backend).
    void postStatusToBackend();
    bool _statusPostFailLogged = false; // log the first failure, not every 30s

    // Supplemental time source, attempted only while charging (see .cpp)
    // - GPS is the primary source and works fine on battery in the field,
    // but often can't get a fix indoors while charging on a desk. Starts
    // the ESP32's SNTP client once per boot/wake cycle; the clock updates
    // asynchronously in the background from then on, same as GPS's own
    // async settimeofday() whenever it happens to get a fix.
    void ntpSyncIfNeeded();
    bool _ntpRequested = false;
};

extern SyncManager syncManager;
