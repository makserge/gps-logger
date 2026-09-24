#include "track_store.h"
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
        Serial.println("[TrackStore] LittleFS mount failed");
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

void TrackStore::startNewSession() {
    _buffer.clear();
    assignSessionIdentity((uint32_t)time(nullptr));
    Serial.printf("[TrackStore] New session: %s\n", _currentSessionPath.c_str());
}

void TrackStore::resumeSession(uint32_t startEpoch) {
    if (startEpoch == 0) return; // nothing to resume
    assignSessionIdentity(startEpoch);
    Serial.printf("[TrackStore] Resumed session: %s\n", _currentSessionPath.c_str());
}

void TrackStore::writeGpxHeader(File &f) {
    f.print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    f.print("<gpx version=\"1.1\" creator=\"esp32c6-gps-logger\" "
            "xmlns=\"http://www.topografix.com/GPX/1/1\">\n");
    f.print(" <trk>\n");
    f.printf("  <name>%s</name>\n", _sessionName.c_str());
    f.print("  <trkseg>\n");
}

void TrackStore::addPoint(const TrackPoint &pt) {
    // Explicit belt-and-braces gate: never write anything, not even to the
    // RAM buffer, until the clock is plausibly synced. main.cpp already
    // only calls addPoint() when gpsReader.hasFix() is true (which implies
    // a synced clock in the overwhelming majority of cases), but this
    // makes the invariant hold regardless of caller behavior - a point
    // with an unsynced timestamp is simply dropped rather than risking
    // another spurious 1970-01-01 session ever being created again.
    if ((time_t)pt.epoch < PLAUSIBLE_EPOCH_FLOOR) {
        Serial.println("[TrackStore] Dropping point - clock not synced yet");
        return;
    }

    // Lazily start a session on the first real point rather than eagerly at
    // boot: a point only exists because GPS has a fix, which means the
    // system clock (set from GPS NMEA data) is reliably synced by now - so
    // the session's date-folder is guaranteed correct, unlike calling this
    // at boot before any fix (which would create a spurious 1970-01-01
    // folder, since the clock reads epoch-0 until GPS syncs it).
    if (_currentSessionPath.isEmpty()) startNewSession();

    if (_buffer.size() >= RAM_TRACKPOINT_CAPACITY) {
        // Ring buffer full and not yet flushed -> force a flush to avoid data loss
        Serial.println("[TrackStore] RAM buffer full, forcing flush");
        flushToFlash();
        if (_buffer.size() >= RAM_TRACKPOINT_CAPACITY) {
            // Flush failed (flash full/corrupt) - behave like a true ring
            // buffer and drop the oldest point instead of growing without
            // bound: unchecked growth would exhaust the heap and crash,
            // losing the ENTIRE in-RAM track instead of just its oldest end.
            _buffer.erase(_buffer.begin());
        }
    }
    _buffer.push_back(pt);
}

bool TrackStore::flushToFlash() {
    // Check the buffer BEFORE touching session state: if nothing has ever
    // been logged (e.g. a periodic/pre-sleep flush fires with zero fixes
    // obtained so far), don't spuriously create a session/folder at all.
    if (_buffer.empty()) return true;
    if (_currentSessionPath.isEmpty()) startNewSession();

    bool fileExists = LittleFS.exists(_currentSessionPath);
    File f;

    if (!fileExists) {
        f = LittleFS.open(_currentSessionPath, "w");
        if (!f) {
            Serial.println("[TrackStore] Failed to create session file");
            return false;
        }
        writeGpxHeader(f);
    } else {
        f = LittleFS.open(_currentSessionPath, "r+");
        if (!f) {
            Serial.println("[TrackStore] Failed to open session file for append");
            return false;
        }
        // The file already existing (checked against flash, not RAM state)
        // means a previous flush already wrote a footer - whether that
        // happened earlier this boot or in a prior wake cycle before a
        // deep sleep. Always seek back past it based on actual file size,
        // rather than an in-RAM flag: RAM state doesn't survive deep sleep,
        // but the file's real size on flash does, so this stays correct
        // across sleep/wake without needing separate bookkeeping (and
        // without risking appending after an old footer, which would
        // silently corrupt the GPX).
        size_t sz = f.size();
        if (sz > GPX_FOOTER_LEN) {
            f.seek(sz - GPX_FOOTER_LEN, SeekSet);
        } else {
            f.seek(sz, SeekSet); // defensive: smaller than expected, just append
        }
    }

    for (const auto &pt : _buffer) {
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

        f.printf("   <trkpt lat=\"%.7f\" lon=\"%.7f\">\n", lat, lon);
        f.printf("    <ele>%.1f</ele>\n", alt);
        f.printf("    <time>%s</time>\n", iso);
        f.printf("    <sat>%u</sat>\n", pt.sats);
        f.printf("    <hdop>%.1f</hdop>\n", hdop);
        f.print("    <extensions>\n");
        f.printf("     <speed>%.2f</speed>\n", speedMs);
        f.printf("     <course>%.1f</course>\n", course);
        f.printf("     <battery>%u</battery>\n", pt.battery_pct);
        f.print("    </extensions>\n");
        f.print("   </trkpt>\n");
    }

    f.print(GPX_FOOTER);
    f.close();

    Serial.printf("[TrackStore] Flushed %u points to %s\n", _buffer.size(), _currentSessionPath.c_str());
    _buffer.clear();
    return true;
}

void TrackStore::endSession() {
    flushToFlash();
    _currentSessionPath = "";
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
        // Deleting the file currently being appended to - clear the active
        // session state so the next point starts a fresh one instead of
        // reopening/recreating a file whose identity we just deleted.
        _currentSessionPath = "";
    }
    if (!LittleFS.exists(path)) return false;
    bool ok = LittleFS.remove(path);
    Serial.printf("[TrackStore] Delete %s: %s\n", path.c_str(), ok ? "ok" : "failed");
    return ok;
}

bool TrackStore::deleteDate(const String &date) {
    auto files = listSessionsForDate(date);
    bool allOk = true;
    for (auto &f : files) {
        if (!deleteSession(date, f)) allOk = false;
    }
    String dir = String(FS_SESSIONS_DIR) + "/" + date;
    if (LittleFS.exists(dir)) {
        if (!LittleFS.rmdir(dir)) allOk = false;
    }
    Serial.printf("[TrackStore] Delete date %s: %s\n", date.c_str(), allOk ? "ok" : "partial/failed");
    return allOk;
}
