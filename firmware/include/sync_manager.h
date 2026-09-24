#pragma once
#include "config.h"
#include <WiFiClient.h>

// Snapshot of the sync machinery's state, for display in the device web UI
// (see WebUi::handleApiStatus). All counts refer to the current wake cycle
// except pendingSessions, which reflects what's actually on flash.
struct SyncStatus {
    bool uploadsGaveUp;     // 10 consecutive upload failures - off until next boot
    uint8_t uploadFails;    // consecutive upload failures so far
    int pendingSessions;    // session files on flash not yet uploaded
    bool syncing;           // an upload pass is running right now
    int syncCurrent;        // files attempted so far in the running/last pass
    int syncTotal;          // pending files at the start of the running/last pass
    int lastUploaded;       // files successfully uploaded in the last pass
    unsigned long lastAttemptMs; // millis() of the last pass start, 0 = none yet
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

    // Clears the per-wake-cycle give-up state and the failure counters.
    // Called when the backend host/port is changed via the web UI - the old
    // backend being unreachable says nothing about the new one, so retries
    // should start fresh without a reboot.
    void resetBackoff() {
        _uploadFailCount = 0;
        _uploadDisabled = false;
    }

    // Push all unsynced sessions to backend via REST. Returns count synced.
    int syncPendingSessions();

    // Forget which sessions have been uploaded. The sync ledger records
    // paths only - NOT which backend they went to - so after the backend
    // host changes, files synced to the OLD backend would be skipped
    // forever ("only new tracks show up on the new server"). Called
    // whenever the backend is (re)configured via the web UI: every file on
    // flash re-uploads to the new target on the next pass, which is safe
    // because uploads are idempotent PUTs.
    void clearSyncState();

    // Called by the web UI's "Sync now" button
    void requestImmediateSync() { _syncRequested = true; }

    // Counting pending sessions means walking the session dirs and parsing
    // sync_state.json once per file - too expensive to redo on every status
    // poll (10s, 2s during a sync pass) and every 30s sync check. This
    // cached variant recomputes at most every few seconds; anything that
    // changes the count (an upload, a deletion) invalidates explicitly, so
    // staleness is bounded and harmless.
    int countPendingSessionsCached();
    void invalidatePendingCache() { _pendingCacheMs = 0; }

    // True while an upload pass is running. The web server is serviced
    // BETWEEN file uploads during a pass (for live progress + reachability),
    // which makes its handlers reentrant into sync state - mutating handlers
    // (delete, OTA, backend change, WiFi reset) check this and refuse with
    // 503 rather than yanking files/state out from under the running pass.
    bool isSyncing() const { return _syncing; }

private:
    WiFiClient _wifiClient;
    unsigned long _lastSyncCheck = 0;
    bool _syncRequested = false;
    // Consecutive REST upload failures: each failed upload blocks the loop
    // for the HTTP connect timeout and fires TX bursts, and with the
    // backend down every pending file fails every sync check, forever.
    // After SYNC_MAX_UPLOAD_FAILURES consecutive failures, uploads (and
    // status posts - same backend) stay off until the next boot/wake cycle;
    // any success resets. Nothing is lost - files stay pending on flash.
    uint8_t _uploadFailCount = 0;
    bool _uploadDisabled = false;
    // Progress bookkeeping for the web UI (see SyncStatus/getStatus())
    bool _syncing = false;
    int _syncCurrent = 0;
    int _syncTotal = 0;
    int _lastUploaded = 0;
    unsigned long _lastAttemptMs = 0;

    int countPendingSessions(); // uncached full walk - see countPendingSessionsCached()
    int _pendingCache = -1;
    unsigned long _pendingCacheMs = 0; // 0 = invalid

    bool uploadSessionFile(const String &date, const String &sessionFile);

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
