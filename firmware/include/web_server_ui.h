#pragma once
#include <WebServer.h>

class WebUi {
public:
    void begin();
    void loop();
    // millis() of the last request served (0 = none yet) - lets main.cpp
    // close a WiFi window nobody is using (see WIFI_IDLE_CLOSE_MS).
    unsigned long lastRequestMs() const { return _lastRequestMs; }

private:
    WebServer _server{80};
    unsigned long _lastRequestMs = 0;

    void handleRoot();
    void handleApiSessions();          // GET /api/sessions -> {date: [files]}
    void handleApiDownload();          // GET /api/download?date=&file=
    void handleApiStatus();            // GET /api/status
    void handleApiSatellites();        // GET /api/satellites
    void handleApiSyncNow();           // POST /api/sync
    void handleApiDelete();            // DELETE /api/sessions?date=[&file=]
    void handleApiResetWifi();         // POST /api/reset-wifi
    void handleApiSetTimezone();       // POST /api/timezone?offsetMinutes=
    void handleApiSetBackend();        // POST /api/backend?host=&port=
    void handleApiSetWifi();           // POST /api/wifi (form body: ssid, pass, ssid2, pass2)
    void handleApiLog();               // GET /api/log[?download=1] - debug log as text
    void handleApiSetDebug();          // POST /api/debug?on=1|0
    void handleApiSetStorage();        // POST /api/storage?reclaim=1|0
    void handleApiShutdown();          // POST /api/shutdown
    // Sends a 503 and returns true when a sync pass is running - mutating
    // handlers call this first, since they are reentrant into the pass (the
    // web server is serviced between file uploads for live progress).
    bool rejectIfSyncing();
    // Host/Origin check against DNS rebinding and cross-site requests (see
    // .cpp). respond=false: only decide (used inside the upload stream).
    bool requestAllowed(bool mutating, bool respond = true);
    void handleOtaUpload();            // POST /update (multipart firmware upload)
};

extern WebUi webUi;
