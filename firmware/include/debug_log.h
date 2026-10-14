#pragma once
#include <Arduino.h>

// All firmware logging goes through `Log` (a Print, so Log.printf/println
// work like Serial's). Every line always goes to Serial, exactly as before.
// While the debug log is switched on (Settings -> Debug log, persisted in
// config.json) each line is ALSO recorded with a timestamp and saved to
// flash (FS_DEBUG_LOG_PATH, rotated at DEBUG_LOG_FILE_MAX_BYTES), so it
// survives restarts, deep sleep and power loss - the web UI shows it via
// GET /api/log. Lines are buffered in RAM and written out every
// DEBUG_LOG_SAVE_INTERVAL_MS, before deep sleep and before any restart, so
// at most the last few seconds before a crash are lost; the crash itself
// is visible as the next boot's reset reason.
//
// Single-task use only (the Arduino loop task, which is also where the web
// server runs) - don't log from ISRs or WiFi/system event callbacks; hand
// data to the loop instead (see WifiConnector's disconnect reason).
class DebugLog : public Print {
public:
    // Call once config is loaded. Until then lines are held in RAM, so the
    // boot banner is recorded too when the log turns out to be enabled.
    void begin(bool enabled);
    void loop();                 // periodic save to flash
    bool enabled() const { return _state == State::On; }
    void setEnabled(bool on);    // caller persists the setting
    void persist();              // write buffered lines to flash now
    void clear();                // delete saved log files + buffer
    // Flash pressure (SyncManager::reclaimSpace), on or off: deletes the
    // older rotated file, and with includeCurrent the current one too.
    // Returns bytes freed.
    size_t releaseSpace(bool includeCurrent);
    size_t storedBytes();        // saved on flash + buffered
    // Lines not written to flash yet (normally none right after persist(),
    // but they stay here while flash is too full to save them).
    const char *buffered(size_t &len) const;

    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t size) override;

private:
    enum class State { Undecided, On, Off };
    State _state = State::Undecided;
    bool _atLineStart = true;
    bool _skippingLine = false;  // buffer full - dropping the rest of this line
    uint32_t _droppedLines = 0;
    size_t _len = 0;
    unsigned long _lastSaveMs = 0;
    void record(uint8_t c);
    void appendLinePrefix();
    bool appendRaw(const char *s, size_t n);
};

extern DebugLog Log;

// Extra detail that's only worth printing while the debug log is on.
#define LOGD(...) do { if (Log.enabled()) Log.printf(__VA_ARGS__); } while (0)
