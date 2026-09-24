#pragma once
#include "config.h"
#include <vector>
#include <FS.h> // declares the fs::File type (used as `File` below) - needed
                // here, not just in track_store.cpp, since any other file
                // that includes this header before LittleFS.h needs it too

// Manages:
//  - RAM ring buffer of TrackPoints for the *current* session
//  - Flushing that buffer to a GPX file on LittleFS every N minutes / low batt / session end
//  - Listing / reading persisted sessions for the web UI and sync
//
// Session identity (which GPX file points get appended to) is intentionally
// NOT eagerly assigned at boot: the system clock isn't set until GPS
// delivers its first fix, so starting a session before that would create a
// spurious 1970-01-01-dated folder. Instead, a session is created lazily,
// the moment the first real point arrives (see addPoint()) - by which point
// the clock is reliably synced, since a point only exists because GPS has
// a fix. To continue an existing trip across a sleep/wake cycle (rather
// than fragmenting it into multiple GPX files), the caller persists
// currentSessionStartEpoch() across deep sleep (e.g. in RTC memory) and
// passes it back into resumeSession() on the next boot - that deterministically
// reconstructs the same date/path without needing to persist strings.
class TrackStore {
public:
    bool begin();

    // Start a brand-new session (new GPX file) with a fresh timestamp.
    // Normally you don't need to call this directly - addPoint() does it
    // lazily on the first real point. Exposed for endSession()/testing.
    void startNewSession();

    // Reconstruct an existing session's date/path from a previously-issued
    // start epoch (see currentSessionStartEpoch()), so points continue
    // appending to the same GPX file after a deep-sleep wake instead of
    // starting a new one. No-op if startEpoch is 0.
    void resumeSession(uint32_t startEpoch);

    // Add a fix to the RAM buffer (applies min-distance/min-interval filtering
    // upstream). Lazily starts a session first if none is active yet.
    void addPoint(const TrackPoint &pt);

    // Flush current RAM buffer to flash, appending to the active session's GPX file.
    // Called periodically (15 min), on low battery, and before sleep/shutdown.
    // No-ops (and starts no session) if the buffer is empty.
    bool flushToFlash();

    // Close out the current session (final flush + mark complete so it shows in calendar)
    void endSession();

    size_t ramPointCount() const { return _buffer.size(); }
    bool ramNeedsFlush() const { return !_buffer.empty(); }

    // ---- Query API used by web UI / sync manager ----
    // Returns list of session date-directories, e.g. ["2026-08-08", "2026-08-07"]
    std::vector<String> listSessionDates();
    // Returns list of session file names (without path) for a given date
    std::vector<String> listSessionsForDate(const String &date);
    // Full path helper
    String sessionPath(const String &date, const String &sessionFile);

    // Delete a single session file. Returns false if it doesn't exist or
    // the delete fails. If it happens to be the currently-active session,
    // clears that state too so the next point starts a fresh one cleanly
    // instead of pointing at a now-deleted file.
    bool deleteSession(const String &date, const String &file);
    // Delete an entire date's worth of sessions (all files, then the
    // now-empty directory itself) - e.g. for clearing out a bad date like
    // a pre-GPS-sync 1970-01-01 folder. Same currently-active-session
    // safeguard as deleteSession().
    bool deleteDate(const String &date);

    String currentSessionPath() const { return _currentSessionPath; }
    // 0 means no session has been started yet this boot/trip.
    uint32_t currentSessionStartEpoch() const { return _sessionStartEpoch; }

private:
    std::vector<TrackPoint> _buffer;
    String _currentSessionPath;
    String _currentSessionDate;
    // Human-readable session identifier, e.g. "2026-08-22_14-03-11" - used
    // as BOTH the filename (with .gpx appended) and the GPX <name> tag
    // content, so a downloaded file's name always matches its track name.
    String _sessionName;
    uint32_t _sessionStartEpoch = 0;

    void writeGpxHeader(File &f);
    String makeSessionFileName();
    void ensureDateDir(const String &date);
    void assignSessionIdentity(uint32_t startEpoch); // shared by startNewSession/resumeSession
};

extern TrackStore trackStore;
