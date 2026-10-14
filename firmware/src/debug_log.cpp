#include "debug_log.h"
#include "config.h"
#include <LittleFS.h>
#include <time.h>
#include <esp_system.h>

DebugLog Log;

static char s_buf[DEBUG_LOG_BUF_BYTES];

static void persistOnRestart() { Log.persist(); }

void DebugLog::begin(bool enabled) {
    _state = enabled ? State::On : State::Off;
    if (!enabled) {
        _len = 0; // boot lines held so far aren't wanted
        return;
    }
    // esp_restart() runs registered shutdown handlers first: covers every
    // ESP.restart() in the firmware (OTA, planned restarts, Reset WiFi)
    // without a persist() call at each site. Deep sleep doesn't run them -
    // PowerManager::enterDeepSleep() calls persist() itself.
    static bool handlerRegistered = false;
    if (!handlerRegistered) {
        esp_register_shutdown_handler(persistOnRestart);
        handlerRegistered = true;
    }
    _lastSaveMs = millis();
}

void DebugLog::setEnabled(bool on) {
    if (on && _state != State::On) {
        begin(true);
        printf("[Debug] Debug log switched on\n");
    } else if (!on && _state == State::On) {
        printf("[Debug] Debug log switched off\n");
        persist();
        _state = State::Off;
    }
}

// The last byte of s_buf is reserved for the newline that terminates a line
// cut off by a full buffer (see record()), so a stored fragment never runs
// into the next line.
static constexpr size_t BUF_CONTENT = sizeof(s_buf) - 1;

bool DebugLog::appendRaw(const char *s, size_t n) {
    if (_len + n > BUF_CONTENT) return false;
    memcpy(s_buf + _len, s, n);
    _len += n;
    return true;
}

void DebugLog::appendLinePrefix() {
    char pre[32];
    time_t now = time(nullptr);
    // Before begin() the configured timezone isn't applied yet (config not
    // loaded), so a wall-clock stamp would silently be UTC - use uptime.
    if (_state != State::Undecided && now >= PLAUSIBLE_EPOCH_FLOOR) {
        struct tm t;
        localtime_r(&now, &t);
        strftime(pre, sizeof(pre), "%d.%m %H:%M:%S ", &t);
    } else {
        unsigned long ms = millis();
        snprintf(pre, sizeof(pre), "+%lu.%lus ", ms / 1000UL, (ms / 100UL) % 10UL);
    }
    // Leave room for at least a short line after the prefix; a line that
    // can't even start is dropped whole (counted).
    if (_len + strlen(pre) + 32 > BUF_CONTENT) {
        _skippingLine = true;
        _droppedLines++;
        return;
    }
    appendRaw(pre, strlen(pre));
}

size_t DebugLog::write(uint8_t c) {
    Serial.write(c);
    record(c);
    return 1;
}

size_t DebugLog::write(const uint8_t *buf, size_t size) {
    Serial.write(buf, size);
    for (size_t i = 0; i < size; i++) record(buf[i]);
    return size;
}

void DebugLog::record(uint8_t c) {
    if (_state == State::Off) return;
    if (_atLineStart) {
        _skippingLine = false;
        appendLinePrefix();
        _atLineStart = false;
    }
    if (!_skippingLine && c != '\r') {
        char ch = (char)c;
        if (!appendRaw(&ch, 1)) {
            s_buf[_len++] = '\n'; // reserved byte - terminate the cut-off line
            _skippingLine = true;
            _droppedLines++;
        }
    }
    if (c == '\n') _atLineStart = true;
}

void DebugLog::persist() {
    if (_state != State::On || (_len == 0 && _droppedLines == 0)) return;
    // Stamped before trying, so a failure is retried at the next save
    // interval instead of on every loop pass.
    _lastSaveMs = millis();
    // Never compete with track data for the last free space - running out
    // of flash is exactly what corrupted tracks once. Lines stay in RAM
    // (visible via /api/log) until space is back; overflow is counted.
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < DEBUG_LOG_MIN_FREE_BYTES) {
        // Give space back, not just stop growing: the older rotated half
        // is the least valuable data on the device when tracks need room.
        if (!LittleFS.exists(FS_DEBUG_LOG_OLD_PATH)) return;
        LittleFS.remove(FS_DEBUG_LOG_OLD_PATH);
        if (LittleFS.totalBytes() - LittleFS.usedBytes() < DEBUG_LOG_MIN_FREE_BYTES) return;
    }
    File f = LittleFS.open(FS_DEBUG_LOG_PATH, "a");
    if (!f) return;
    f.write((const uint8_t *)s_buf, _len);
    if (_droppedLines) {
        f.printf("[Debug] %lu line(s) dropped - log buffer was full\n", (unsigned long)_droppedLines);
        _droppedLines = 0;
    }
    size_t size = f.size();
    f.close();
    _len = 0;
    if (size > DEBUG_LOG_FILE_MAX_BYTES) {
        if (LittleFS.exists(FS_DEBUG_LOG_OLD_PATH)) LittleFS.remove(FS_DEBUG_LOG_OLD_PATH);
        LittleFS.rename(FS_DEBUG_LOG_PATH, FS_DEBUG_LOG_OLD_PATH);
    }
}

void DebugLog::loop() {
    if (_state != State::On || _len == 0) return;
    unsigned long since = millis() - _lastSaveMs;
    // Early save when 3/4 full, but at most every 2 s - if saving isn't
    // possible (flash nearly full) the buffer stays full, and this must
    // not turn into a flash-space check on every 10 ms loop pass.
    if (since >= DEBUG_LOG_SAVE_INTERVAL_MS || (_len > BUF_CONTENT * 3 / 4 && since >= 2000)) {
        persist();
    }
}

void DebugLog::clear() {
    for (const char *p : {FS_DEBUG_LOG_OLD_PATH, FS_DEBUG_LOG_PATH}) {
        if (LittleFS.exists(p)) LittleFS.remove(p);
    }
    _len = 0;
    _droppedLines = 0;
}

size_t DebugLog::releaseSpace(bool includeCurrent) {
    size_t freed = 0;
    for (const char *p : {FS_DEBUG_LOG_OLD_PATH, FS_DEBUG_LOG_PATH}) {
        if (!includeCurrent && strcmp(p, FS_DEBUG_LOG_PATH) == 0) continue;
        if (!LittleFS.exists(p)) continue;
        File f = LittleFS.open(p, "r");
        size_t sz = f ? f.size() : 0;
        if (f) f.close();
        if (LittleFS.remove(p)) freed += sz;
    }
    return freed;
}

const char *DebugLog::buffered(size_t &len) const {
    len = _len;
    return s_buf;
}

size_t DebugLog::storedBytes() {
    size_t total = _len;
    for (const char *p : {FS_DEBUG_LOG_OLD_PATH, FS_DEBUG_LOG_PATH}) {
        if (!LittleFS.exists(p)) continue; // open() on a missing file prints an [E] line
        File f = LittleFS.open(p, "r");
        if (f) { total += f.size(); f.close(); }
    }
    return total;
}
