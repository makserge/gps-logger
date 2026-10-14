#include "track_store.h"
#include "debug_log.h"
#include <LittleFS.h>
#include <time.h>
#include <algorithm>

TrackStore trackStore;

// -----------------------------------------------------------------------
// GPX append strategy:
// Writing a single well-formed GPX file incrementally is awkward because
// the closing </trkseg></trk></gpx> tags must be the last bytes written.
// We solve this cheaply and robustly:
//   1. On flush, if the file doesn't exist yet, write header + open tags.
//   2. Otherwise, open in "r+" mode, seek to end, and *rewind past* the
//      footer marker (a fixed-length placeholder) before appending.
//   3. After appending new <trkpt> elements, rewrite the footer.
// This keeps the file valid GPX 1.1 after every flush, so it's always
// readable by GPX tools (GPX Studio, OsmAnd, Garmin BaseCamp, etc.) even
// if the device loses power mid-session.
// -----------------------------------------------------------------------

static const char *GPX_FOOTER = "  </trkseg>\n </trk>\n</gpx>\n";
// Footer is fixed length so we can always seek back over it. Derived from
// the string itself, never hand-counted: a hardcoded 28 shipped here once
// while the footer is actually 27 bytes, making every append-seek land one
// byte early and silently overwrite the byte before the footer.
static const size_t GPX_FOOTER_LEN = strlen(GPX_FOOTER);

bool TrackStore::begin() {
    if (!LittleFS.begin(true)) {
        Log.println("[TrackStore] LittleFS mount failed");
        return false;
    }
    if (!LittleFS.exists(FS_SESSIONS_DIR)) {
        LittleFS.mkdir(FS_SESSIONS_DIR);
    }
    _buffer.reserve(RAM_TRACKPOINT_CAPACITY);
    return true;
}

String TrackStore::makeSessionFileName() {
    return _sessionName + ".gpx";
}

void TrackStore::ensureDateDir(const String &date) {
    String dir = String(FS_SESSIONS_DIR) + "/" + date;
    if (!LittleFS.exists(dir)) {
        LittleFS.mkdir(dir);
    }
}

// Shared by startNewSession() (fresh epoch) and resumeSession() (a
// previously-issued epoch) - both just need to derive the date-folder and
// file path from an epoch and set up directories.
void TrackStore::assignSessionIdentity(uint32_t startEpoch) {
    _sessionStartEpoch = startEpoch;

    // Local time (via the TZ POSIX string applied by ConfigManager, DST
    // included) for the folder/filename, so trips are grouped and named by
    // local calendar day/time rather than UTC - a trip starting at 11pm
    // local in a UTC+ zone would otherwise show up filed under "tomorrow".
    // This is naming/grouping only; the actual <trkpt><time> values written
    // into the GPX body stay UTC always (see the trkpt-writing loop below),
    // per the GPX spec and for unambiguous interchange with other tools.
    time_t t = startEpoch;
    struct tm tmStart;
    localtime_r(&t, &tmStart);
    char dateBuf[11];
    snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
              tmStart.tm_year + 1900, tmStart.tm_mon + 1, tmStart.tm_mday);
    _currentSessionDate = String(dateBuf);
    ensureDateDir(_currentSessionDate);

    // Filesystem/URL-safe, colon-free timestamp - used identically as the
    // filename (with .gpx appended) and the GPX <name> tag, so what a user
    // downloads always matches the track's name inside the file.
    char nameBuf[20];
    snprintf(nameBuf, sizeof(nameBuf), "%04d-%02d-%02d_%02d-%02d-%02d",
              tmStart.tm_year + 1900, tmStart.tm_mon + 1, tmStart.tm_mday,
              tmStart.tm_hour, tmStart.tm_min, tmStart.tm_sec);
    _sessionName = String(nameBuf);

    _currentSessionPath = String(FS_SESSIONS_DIR) + "/" + _currentSessionDate + "/" + makeSessionFileName();
}

void TrackStore::startNewSession(uint32_t startEpoch) {
    // The buffer is deliberately NOT cleared: it can still hold points a
    // failed flush kept (see flushToFlash) - they go into this session
    // rather than being dropped.
    assignSessionIdentity(startEpoch ? startEpoch : (uint32_t)time(nullptr));
    _sessionPoints = 0;
    Log.printf("[TrackStore] New session: %s\n", _currentSessionPath.c_str());
}

void TrackStore::resumeSession(uint32_t startEpoch) {
    if (startEpoch == 0) return; // nothing to resume
    assignSessionIdentity(startEpoch);
    _sessionPoints = 1000000; // already a real trip - never treated as noise
    Log.printf("[TrackStore] Resumed session: %s\n", _currentSessionPath.c_str());
}

// snprintf result -> length to write: never past the buffer (a truncated
// line can't happen with these fixed-size fields, but must not overread).
static int fitLen(int n, size_t cap) {
    if (n < 0) return -1;
    return n >= (int)cap ? (int)cap - 1 : n;
}

bool TrackStore::addPoint(const TrackPoint &pt) {
    // Explicit belt-and-braces gate: never write anything, not even to the
    // RAM buffer, until the clock is plausibly synced. main.cpp already
    // only calls addPoint() when gpsReader.hasFix() is true (which implies
    // a synced clock in the overwhelming majority of cases), but this
    // makes the invariant hold regardless of caller behavior - a point
    // with an unsynced timestamp is simply dropped rather than risking
    // another spurious 1970-01-01 session ever being created again.
    if ((time_t)pt.epoch < PLAUSIBLE_EPOCH_FLOOR) {
        // Rate-limited: with a fix but no valid date this fires on every
        // GPS sentence (twice a second).
        static unsigned long lastDropLogMs = 0;
        static uint32_t dropped = 0;
        dropped++;
        if (lastDropLogMs == 0 || millis() - lastDropLogMs >= 60000UL) {
            Log.printf("[TrackStore] Dropping points - clock not synced yet (%lu so far)\n", (unsigned long)dropped);
            lastDropLogMs = millis();
        }
        return false;
    }

    // Lazily start a session on the first real point rather than eagerly at
    // boot: a point only exists because GPS has a fix, which means the
    // system clock (set from GPS NMEA data) is reliably synced by now - so
    // the session's date-folder is guaranteed correct, unlike calling this
    // at boot before any fix (which would create a spurious 1970-01-01
    // folder, since the clock reads epoch-0 until GPS syncs it).
    if (_currentSessionPath.isEmpty()) startNewSession(pt.epoch);

    if (_buffer.size() >= RAM_TRACKPOINT_CAPACITY) {
        // Buffer full: try a flush - but after a failed one only every
        // FLUSH_RETRY_BACKOFF_MS. Retrying on every point (every 3 s) with
        // the flash full cost a full reclaim walk plus a doomed write of all
        // free blocks each time.
        if (_lastFlushFailMs == 0 || millis() - _lastFlushFailMs >= FLUSH_RETRY_BACKOFF_MS) {
            flushToFlash();
        }
        if (_buffer.size() >= RAM_TRACKPOINT_CAPACITY) {
            // Still full (flash full/corrupt) - behave like a true ring
            // buffer and drop the oldest point instead of growing without
            // bound: unchecked growth would exhaust the heap and crash,
            // losing the ENTIRE in-RAM track instead of just its oldest end.
            _buffer.erase(_buffer.begin());
        }
    }
    _buffer.push_back(pt);
    _sessionPoints++;
    return true;
}

bool TrackStore::flushToFlash() {
    // Check the buffer BEFORE touching session state: if nothing has ever
    // been logged (e.g. a periodic/pre-sleep flush fires with zero fixes
    // obtained so far), don't spuriously create a session/folder at all.
    if (_buffer.empty()) return true;
    // Make room first (SyncManager::reclaimSpace) - on every flush, not
    // only while a backend is reachable, so an offline trip can still use
    // the space of sessions already stored on a backend. It's told how
    // much this flush needs, which can exceed the normal reserve after a
    // long offline stretch.
    if (_beforeFlushHook) _beforeFlushHook(_buffer.size() * GPX_BYTES_PER_POINT + 1024);
    if (_currentSessionPath.isEmpty()) startNewSession(_buffer.front().epoch);
    // The date folder can have vanished since the session started (deleted
    // from the web UI, or removed as empty by an older cleanup) - without
    // it every open() below fails and the whole trip would stay unsaved.
    ensureDateDir(_currentSessionDate);

    // In chunks, each committed on its own: a flush that doesn't fit in
    // the remaining space still stores what does, instead of all-or-nothing.
    size_t total = _buffer.size();
    size_t stored = 0;
    while (!_buffer.empty()) {
        size_t n = std::min(_buffer.size(), (size_t)FLUSH_CHUNK_POINTS);
        if (!flushChunk(n)) {
            // Logged once per failure streak, then at most once a minute.
            static unsigned long lastFailLogMs = 0;
            if (_lastFlushFailMs == 0 || millis() - lastFailLogMs >= 60000UL) {
                Log.printf("[TrackStore] Flush FAILED for %s (flash full?) - %u points kept in RAM\n",
                           _currentSessionPath.c_str(), (unsigned)_buffer.size());
                lastFailLogMs = millis();
            }
            _lastFlushFailMs = millis();
            if (_lastFlushFailMs == 0) _lastFlushFailMs = 1;
            if (stored) Log.printf("[TrackStore] Flushed %u of %u points to %s\n",
                                   (unsigned)stored, (unsigned)total, _currentSessionPath.c_str());
            return false;
        }
        _buffer.erase(_buffer.begin(), _buffer.begin() + n);
        stored += n;
    }
    _lastFlushFailMs = 0;
    Log.printf("[TrackStore] Flushed %u points to %s\n", (unsigned)total, _currentSessionPath.c_str());
    return true;
}

// Writes the first `count` buffered points (plus header if needed, plus
// footer) in one open/close and verifies the result. Does NOT touch the
// buffer - the caller removes the points only on success.
bool TrackStore::flushChunk(size_t count) {
    // Appending needs a file that actually holds a GPX body. A 0-byte (or
    // footer-sized) file is what a failed FIRST flush leaves behind -
    // LittleFS commits the directory entry at open("w") even when the data
    // write then fails - and appending to it wrote points with no GPX
    // header at all. Such a file is rewritten from scratch.
    size_t existing = 0;
    if (LittleFS.exists(_currentSessionPath)) {
        File e = LittleFS.open(_currentSessionPath, "r");
        if (e) { existing = e.size(); e.close(); }
    }
    bool append = existing > GPX_FOOTER_LEN;

    File f;
    size_t baseSize = 0;   // bytes kept from the existing file (before its footer)
    size_t written = 0;    // bytes this chunk wrote
    bool writeOk = true;
    auto put = [&](const char *s, int len) {
        if (len < 0) { writeOk = false; return; }
        size_t n = (size_t)len;
        size_t w = f.write((const uint8_t *)s, n);
        written += w;
        if (w != n) writeOk = false;
    };

    if (!append) {
        f = LittleFS.open(_currentSessionPath, "w");
        if (!f) {
            Log.println("[TrackStore] Failed to create session file");
            return false;
        }
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<gpx version=\"1.1\" creator=\"esp32c6-gps-logger\" "
                         "xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
                         " <trk>\n  <name>%s</name>\n  <trkseg>\n", _sessionName.c_str());
        put(hdr, fitLen(n, sizeof(hdr)));
    } else {
        f = LittleFS.open(_currentSessionPath, "r+");
        if (!f) {
            Log.println("[TrackStore] Failed to open session file for append");
            return false;
        }
        // The file's real size on flash (not RAM state, which doesn't
        // survive deep sleep) says where the previous footer starts - seek
        // back over it and overwrite it with the new points + a new footer.
        baseSize = existing - GPX_FOOTER_LEN;
        f.seek(baseSize, SeekSet);
    }

    char line[160];
    for (size_t i = 0; i < count && writeOk; i++) {
        const TrackPoint &pt = _buffer[i];
        double lat = pt.lat_e7 / 1e7;
        double lon = pt.lon_e7 / 1e7;
        double alt = pt.alt_dm / 10.0;
        double speedMs = pt.speed_cms / 100.0;
        double course = pt.course_dd / 10.0;
        double hdop = pt.hdop_x10 / 10.0;

        time_t t = pt.epoch;
        struct tm tmPt;
        gmtime_r(&t, &tmPt);
        char iso[25];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmPt);

        int n = snprintf(line, sizeof(line),
                         "   <trkpt lat=\"%.7f\" lon=\"%.7f\">\n    <ele>%.1f</ele>\n    <time>%s</time>\n",
                         lat, lon, alt, iso);
        put(line, fitLen(n, sizeof(line)));
        n = snprintf(line, sizeof(line),
                     "    <sat>%u</sat>\n    <hdop>%.1f</hdop>\n    <extensions>\n     <speed>%.2f</speed>\n",
                     pt.sats, hdop, speedMs);
        put(line, fitLen(n, sizeof(line)));
        n = snprintf(line, sizeof(line),
                     "     <course>%.1f</course>\n     <battery>%u</battery>\n    </extensions>\n   </trkpt>\n",
                     course, pt.battery_pct);
        put(line, fitLen(n, sizeof(line)));
    }

    if (writeOk) put(GPX_FOOTER, (int)GPX_FOOTER_LEN);
    f.close();

    // Verify against the file's real size: a write that fails on a full
    // filesystem is discarded by LittleFS at close (the previously
    // committed, still-valid version stays), so short writes and a size
    // that didn't come out right both mean "not stored".
    size_t expected = baseSize + written;
    size_t actual = 0;
    File chk = LittleFS.open(_currentSessionPath, "r");
    if (chk) { actual = chk.size(); chk.close(); }
    return writeOk && actual == expected;
}

bool TrackStore::endSession() {
    // Only close the session once its points are actually stored - a
    // failed flush keeps the path so the next flush retries into the same
    // file instead of the points being orphaned.
    if (!flushToFlash()) return false;
    _currentSessionPath = "";
    _currentSessionDate = "";
    _sessionStartEpoch = 0;
    return true;
}

void TrackStore::discardCurrentSession() {
    _buffer.clear();
    _currentSessionPath = "";
    _currentSessionDate = "";
    _sessionStartEpoch = 0;
}

std::vector<String> TrackStore::listSessionDates() {
    std::vector<String> out;
    File root = LittleFS.open(FS_SESSIONS_DIR);
    if (!root || !root.isDirectory()) return out;
    File entry = root.openNextFile();
    while (entry) {
        if (entry.isDirectory()) {
            String name = entry.name();
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            out.push_back(name);
        }
        entry = root.openNextFile();
    }
    // newest first
    std::sort(out.begin(), out.end(), [](const String &a, const String &b) { return a > b; });
    return out;
}

std::vector<String> TrackStore::listSessionsForDate(const String &date) {
    std::vector<String> out;
    String dir = String(FS_SESSIONS_DIR) + "/" + date;
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return out;
    File entry = d.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            out.push_back(name);
        }
        entry = d.openNextFile();
    }
    return out;
}

String TrackStore::sessionPath(const String &date, const String &sessionFile) {
    return String(FS_SESSIONS_DIR) + "/" + date + "/" + sessionFile;
}

bool TrackStore::deleteSession(const String &date, const String &file) {
    String path = sessionPath(date, file);
    if (path == _currentSessionPath) {
        // Deleting the session being recorded: drop its unsaved points and
        // its identity too. Keeping the buffer made the next flush write it
        // (or an empty GPX) under a new name; keeping the start epoch let a
        // planned restart resume - i.e. recreate - the deleted file.
        discardCurrentSession();
    }
    if (!LittleFS.exists(path)) return false;
    bool ok = LittleFS.remove(path);
    Log.printf("[TrackStore] Delete %s: %s\n", path.c_str(), ok ? "ok" : "failed");
    return ok;
}

bool TrackStore::deleteDate(const String &date) {
    // The live session may not have a file yet (points still in RAM) -
    // deleting its date deletes it too.
    if (date == _currentSessionDate) discardCurrentSession();
    auto files = listSessionsForDate(date);
    bool allOk = true;
    for (auto &f : files) {
        if (!deleteSession(date, f)) allOk = false;
    }
    String dir = String(FS_SESSIONS_DIR) + "/" + date;
    if (LittleFS.exists(dir)) {
        if (!LittleFS.rmdir(dir)) allOk = false;
    }
    Log.printf("[TrackStore] Delete date %s: %s\n", date.c_str(), allOk ? "ok" : "partial/failed");
    return allOk;
}
