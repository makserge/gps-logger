#include "gps_reader.h"
#include "debug_log.h"
#include <time.h>
#include <driver/gpio.h>
#include <cstring>
#include <cstdlib>

GpsReader gpsReader;

// UTC-only civil-date -> epoch conversion with no libc/TZ dependency at
// all - deliberately NOT mktime() (TZ-sensitive - see the comment at its
// call site below) or timegm() (not reliably available across ESP32
// Arduino targets/libc variants; confirmed missing on this esp32-c6
// toolchain: "'timegm' was not declared in this scope"). This is Howard
// Hinnant's well-known "days_from_civil" algorithm - correct for any
// proleptic-Gregorian calendar date, which is all GPS will ever report.
static time_t utcTimeFromTm(const struct tm &t) {
    int y = t.tm_year + 1900;
    int m = t.tm_mon + 1;
    int d = t.tm_mday;
    y -= (m <= 2) ? 1 : 0;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    unsigned mp = (unsigned)((m + 9) % 12);                 // [0, 11], Mar=0..Feb=11
    unsigned doy = (153 * mp + 2) / 5 + (unsigned)d - 1;    // [0, 365]
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   // [0, 146096]
    long days = era * 146097 + (long)doe - 719468;          // days since 1970-01-01
    return (time_t)days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

void GpsReader::begin() {
    pinMode(GPS_POWER_PIN, OUTPUT);
    powerOn(); // drives the module's power switch on and waits for it to settle
    // The loop blocks for seconds at a time during sync passes (HTTP
    // timeouts, JSON ledger work); at 9600 baud (~960 B/s) the default
    // 256-byte RX buffer overflows in ~0.3 s and whole fixes are lost.
    // Must be set before begin().
    _serial.setRxBufferSize(GPS_UART_RX_BUFFER_BYTES);
    _serial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

void GpsReader::powerOn() {
    digitalWrite(GPS_POWER_PIN, GPS_POWER_ON_LEVEL);
    delay(GPS_POWER_STABILIZE_MS);
}

void GpsReader::powerOff() {
    digitalWrite(GPS_POWER_PIN, !GPS_POWER_ON_LEVEL);
}

void GpsReader::end() {
    _serial.end();
    pinMode(GPS_RX_PIN, INPUT); // ESP RX <- GPS TX; harmless either way, tri-stated for symmetry/lowest leakage
    pinMode(GPS_TX_PIN, INPUT); // ESP TX -> GPS RX; the pin that actually matters here
}

void GpsReader::feedGsvByte(char c) {
    if (c == '\n' || _gsvLineLen >= sizeof(_gsvLineBuf) - 1) {
        _gsvLineBuf[_gsvLineLen] = '\0';
        if (_gsvLineLen > 5 && _gsvLineBuf[0] == '$' &&
            _gsvLineBuf[3] == 'G' && _gsvLineBuf[4] == 'S' && _gsvLineBuf[5] == 'V') {
            parseGsvLine(_gsvLineBuf);
        }
        _gsvLineLen = 0;
    } else if (c != '\r') {
        _gsvLineBuf[_gsvLineLen++] = c;
    }
}

// Splits the next comma-delimited field out of *cursor, mutating the
// buffer in place (comma -> '\0') and advancing *cursor past it. Unlike
// strtok_r, this correctly returns an empty string "" for a blank field
// between two adjacent commas rather than silently skipping over it -
// NMEA sentences (SNR in GSV among others) rely on exactly that to mean
// "field present but not reported", which strtok_r cannot represent
// (it treats consecutive delimiters as one, which would silently shift
// every field after the first blank one). Returns nullptr only once
// there's genuinely no more string left to split (past the last field).
static char *nextNmeaField(char **cursor) {
    if (!*cursor) return nullptr;
    char *field = *cursor;
    char *comma = strchr(*cursor, ',');
    if (comma) {
        *comma = '\0';
        *cursor = comma + 1;
    } else {
        *cursor = nullptr; // this was the last field in the string
    }
    return field;
}

// Parses one GSV ("satellites in view") sentence, e.g.:
//   $GPGSV,3,1,11,10,63,137,42,07,61,034,45,05,59,318,42,08,47,073,42*74
// Fields after the sentence ID: total messages this cycle, this
// message's number (1-based), satellites in view (cycle total), then
// repeating groups of 4 - PRN, elevation, azimuth, SNR - up to 4 groups
// per sentence, continuing across sentences until the cycle's total is
// covered. SNR is blank ("") for a satellite that's in view but not
// currently tracked, not a real 0 dB-Hz reading - kept as -1 to mark
// that distinction rather than silently reporting a fake reading. Talker
// ID (the 2 chars right after '$') is deliberately not checked here -
// this module only reports GPS, but parsing any "*GSV" sentence
// generically costs nothing and keeps this working unmodified if a
// module with other constellations is ever swapped in.
void GpsReader::parseGsvLine(char *line) {
    char *star = strchr(line, '*');
    if (star) *star = '\0'; // drop the checksum - not needed here, GSV is
                             // low-stakes display data, not something we
                             // act on the way a corrupt fix would matter
    char *cursor = line;
    nextNmeaField(&cursor);                    // sentence ID, e.g. "$GPGSV" - unused
    char *totalTok = nextNmeaField(&cursor);
    char *numTok   = nextNmeaField(&cursor);
    nextNmeaField(&cursor);                    // satellites-in-view total - unused
                                                // (_pendingCount ends up being
                                                // the same number once the
                                                // cycle completes, derived
                                                // rather than trusted verbatim)
    if (!totalTok || !numTok) return;
    int msgTotal = atoi(totalTok);
    int msgNum = atoi(numTok);
    if (msgNum == 1) _pendingCount = 0; // start of a new cycle

    for (int i = 0; i < 4; i++) {
        char *prnTok = nextNmeaField(&cursor);
        char *elevTok = nextNmeaField(&cursor);
        char *azTok = nextNmeaField(&cursor);
        char *snrTok = nextNmeaField(&cursor); // "" if untracked, per the
                                                // function comment above -
                                                // properly distinguishable
                                                // from "no field left" now
        if (!prnTok || !elevTok || !azTok) break; // no more full groups left
        if (_pendingCount >= MAX_TRACKED_SATS) break;
        SatelliteInfo &sat = _pendingSats[_pendingCount++];
        sat.prn = (uint8_t)atoi(prnTok);
        sat.elevation = (uint8_t)atoi(elevTok);
        sat.azimuth = (uint16_t)atoi(azTok);
        sat.snr = (snrTok && snrTok[0] != '\0') ? (int8_t)atoi(snrTok) : (int8_t)-1;
    }

    if (msgNum >= msgTotal) { // last sentence in the cycle - publish it
        memcpy(_sats, _pendingSats, _pendingCount * sizeof(SatelliteInfo));
        _satCount = _pendingCount;
    }
}

static int twoDigits(const char *p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

// Sets the system clock from an RMC sentence's own time, status and date
// fields - never from TinyGPS++'s combined date/time objects. Those are
// committed by different sentences (time by GGA too, the date only by RMC),
// and this module sends GGA first: at 00:00:00 UTC a fresh GGA time paired
// with yesterday's RMC date set the clock back 24 h, and no age/flag
// heuristic survives a UART backlog, where both sentences are parsed within
// milliseconds of each other. Status must be 'A' (valid fix; a 'V' sentence
// can carry a stale or unset clock), and the year must be plausible (a
// cold start reports an empty or bogus date).
void GpsReader::syncClockFromRmc() {
    for (auto &r : _rmc) {
        if (!(r.time.isUpdated() && r.status.isUpdated() && r.date.isUpdated())) continue;
        const char *tm = r.time.value();   // hhmmss(.sss)
        const char *st = r.status.value(); // A / V
        const char *dt = r.date.value();   // ddmmyy
        if (st[0] != 'A' || strlen(tm) < 6 || strlen(dt) != 6) continue;
        int hh = twoDigits(tm), mi = twoDigits(tm + 2), ss = twoDigits(tm + 4);
        int dd = twoDigits(dt), mo = twoDigits(dt + 2), yy = twoDigits(dt + 4);
        if (hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60
                || dd < 1 || dd > 31 || mo < 1 || mo > 12 || yy < 20) continue;
        struct tm t{};
        t.tm_year = 2000 + yy - 1900;
        t.tm_mon  = mo - 1;
        t.tm_mday = dd;
        t.tm_hour = hh;
        t.tm_min  = mi;
        t.tm_sec  = ss;
        // utcTimeFromTm(), not mktime()/timegm(): GPS date/time fields are
        // always UTC. mktime() interprets struct tm as LOCAL time under
        // whatever TZ is currently set process-wide, so once a timezone is
        // selected in the web UI (ConfigManager::applyTimezone()) it would
        // shift the epoch by the UTC offset; timegm() isn't reliably
        // declared across ESP32 Arduino targets (missing on this esp32-c6
        // toolchain) - utcTimeFromTm() does the same with no TZ dependency.
        time_t epoch = utcTimeFromTm(t);
        time_t before = time(nullptr);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        _lastClockSetEpoch = (uint32_t)epoch;
        if (!_clockSetThisBoot) { // the accumulated error since the last sync - logged by main.cpp
            _clockSetThisBoot = true;
            _firstSyncStepS = (long)(epoch - before);
            _firstSyncPending = true;
        }
    }
}

bool GpsReader::update() {
    bool newFix = false;
    while (_serial.available() > 0) {
        char c = _serial.read();
        feedGsvByte(c); // independent of TinyGPS++ below - see gps_reader.h
#ifdef GPS_DEBUG_RAW_NMEA
        if (c == '\n' || _debugLineLen >= sizeof(_debugLineBuf) - 1) {
            _debugLineBuf[_debugLineLen] = '\0';
            // Serial only: ~1 KB/s of NMEA would flood the debug log.
            if (_debugLineLen > 0) Serial.printf("[GPS RAW] %s\n", _debugLineBuf);
            _debugLineLen = 0;
        } else if (c != '\r') {
            _debugLineBuf[_debugLineLen++] = c;
        }
#endif
        if (_gps.encode(c)) {
            syncClockFromRmc(); // first, so a point from this very sentence uses the new time
            if (_gps.location.isUpdated() && _gps.location.isValid()) newFix = true;
        }
    }
    return newFix;
}

TrackPoint GpsReader::currentAsTrackPoint(uint8_t batteryPct) const {
    TrackPoint pt{};
    pt.epoch = (uint32_t)time(nullptr);
    pt.lat_e7 = (int32_t)(_gps.location.lat() * 1e7);
    pt.lon_e7 = (int32_t)(_gps.location.lng() * 1e7);
    // Clamped: decimetres in int16 overflow above 3276.7 m (and would wrap
    // to large negative altitudes).
    pt.alt_dm = _gps.altitude.isValid()
        ? (int16_t)constrain(_gps.altitude.meters() * 10.0, -32768.0, 32767.0) : 0;
    pt.speed_cms = _gps.speed.isValid() ? (uint16_t)(_gps.speed.mps() * 100) : 0;
    pt.course_dd = _gps.course.isValid() ? (uint16_t)(_gps.course.deg() * 10) : 0;
    pt.sats = _gps.satellites.isValid() ? (uint8_t)min<uint32_t>(_gps.satellites.value(), 255) : 0;
    pt.hdop_x10 = _gps.hdop.isValid() ? (uint8_t)min<uint32_t>((uint32_t)(_gps.hdop.hdop() * 10), 255) : 255;
    pt.battery_pct = batteryPct;
    return pt;
}

double GpsReader::distanceFromMeters(double lat, double lon) const {
    if (!_gps.location.isValid()) return 0.0;
    return TinyGPSPlus::distanceBetween(lat, lon, _gps.location.lat(), _gps.location.lng());
}
