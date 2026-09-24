#pragma once
#include <WebServer.h>

class WebUi {
public:
    void begin();
    void loop();

private:
    WebServer _server{80};

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
    // Sends a 503 and returns true when a sync pass is running - mutating
    // handlers call this first, since they are reentrant into the pass (the
    // web server is serviced between file uploads for live progress).
    bool rejectIfSyncing();
    void handleOtaUpload();            // POST /update (multipart firmware upload)
};

extern WebUi webUi;
