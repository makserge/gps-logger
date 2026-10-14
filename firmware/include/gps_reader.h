#pragma once
#include "config.h"
#include <TinyGPS++.h>
#include <HardwareSerial.h>

// One entry per satellite currently reported in the GPS module's GSV
// ("satellites in view") sentences. This is a genuinely different thing
// from GpsReader::satellites() below: that's the COUNT of satellites
// actually used in the current position fix (from GSA/GGA); this is the
// full list of satellites the module can see at all, tracked or not,
// each with its own signal level - closer to what people mean by
// "RSSI" for GPS, though the correct term is C/N0 (carrier-to-noise
// density, dB-Hz) - GPS doesn't have a classic RF RSSI, since NMEA never
// exposes raw received power, only this derived per-satellite figure.
struct SatelliteInfo {
    uint8_t prn = 0;       // satellite ID. Numbering is constellation-specific:
                            // GPS 1-32, SBAS 33-64, GLONASS 65-96, Galileo
                            // 301-336, BeiDou 201-263 (approximate ranges -
                            // varies a bit by receiver/NMEA revision).
    uint8_t elevation = 0; // degrees above horizon, 0-90
    uint16_t azimuth = 0;  // degrees from true north, 0-359
    int8_t snr = -1;       // dB-Hz, roughly 0-60; -1 = in view but not
                            // currently tracked (GSV omits SNR for these)
};

class GpsReader {
public:
    static constexpr size_t MAX_TRACKED_SATS = 32; // generous headroom
                                                     // for a GPS-only receiver;
                                                     // cheap to keep this large

    void begin();

    // Hardware power control via GPS_POWER_PIN. Note: this drives an
    // AO3401 P-MOSFET (Q1) gating the module's main VCC, NOT the
    // ATGM336H's own pin 5 (ON/OFF) - see the wiring/tradeoff comments on
    // GPS_POWER_PIN and GPS_POWER_ON_LEVEL in config.h for why the levels
    // are inverted from what driving pin 5 directly would need, and for
    // the hot-start-vs-cold-start expectations of cutting main VCC versus
    // using the module's own shutoff pin:
    //  - powerOn() drives GPS_POWER_PIN to GPS_POWER_ON_LEVEL and waits
    //    GPS_POWER_STABILIZE_MS for the module to come up before UART is
    //    expected to respond.
    //  - powerOff() drives GPS_POWER_PIN to the opposite level, cutting
    //    the module's VCC via Q1.
    void powerOn();
    void powerOff();

    // Fully releases the UART peripheral and tri-states its two pins
    // (INPUT, no pull) so the ESP32 stops driving current into the GPS
    // module's RX line once it's powered down via powerOff(). Matters
    // because a HardwareSerial TX pin left driven HIGH can back-feed a
    // small current through a peripheral's input protection diodes even
    // after that peripheral's own supply/enable pin says "off" - enough,
    // on some modules, to keep an internal rail alive and prevent the
    // module from ever reaching its real sleep current. Call this after
    // powerOff() and before entering deep sleep; call begin() again on
    // the next wake to bring the UART back up (already done unconditionally
    // every wake in setup(), since deep sleep re-runs it from scratch).
    void end();

    // Call frequently from loop(); feeds bytes into TinyGPS++.
    // Returns true if a *new* valid fix was parsed this call.
    bool update();

    bool hasFix() const { return _gps.location.isValid() && _gps.location.age() < 5000; }
    // The first time this boot set the clock from GPS: how far the clock
    // was off (GPS minus device, seconds). Returns true once, then false.
    // Epoch the clock was last set to from GPS (0 = not this boot).
    uint32_t lastClockSetEpoch() const { return _lastClockSetEpoch; }
    bool takeFirstSyncStep(long &stepS) {
        if (!_firstSyncPending) return false;
        _firstSyncPending = false;
        stepS = _firstSyncStepS;
        return true;
    }

    // hasFix() alone only means "a recent fix exists" - a receiver keeps
    // producing a "valid" fix while badly multipathing indoors (signal
    // bouncing off walls/roof), just a wildly imprecise one: the reported
    // position can jump tens of meters between updates while the device
    // is actually sitting still on a shelf. HDOP (Horizontal Dilution of
    // Precision - lower is better) and the satellite count actually used
    // in the fix are what distinguish reception good enough to trust a
    // computed displacement from indoor chatter. Use this - not hasFix()
    // - anywhere a GPS-measured distance decides "did the device actually
    // move" (e.g. the keep-awake heuristic in main.cpp's loop()).
    bool hasGoodFix() const {
        return hasFix()
            && _gps.hdop.isValid() && _gps.hdop.hdop() <= GPS_MOVEMENT_MAX_HDOP
            && _gps.satellites.isValid() && _gps.satellites.value() >= GPS_MOVEMENT_MIN_SATS;
    }
    // hasGoodFix() plus the indoor gate (see GPS_TRUSTED_SPEED_MPS): at
    // driving speed any good fix, slower only one with open-sky quality.
    // This is what logging and "parked" decisions use.
    bool hasTrustedFix() const {
        if (!hasGoodFix()) return false;
        if (speedMps() >= GPS_TRUSTED_SPEED_MPS) return true;
        return _gps.satellites.value() >= GPS_STATIC_MIN_SATS && _gps.hdop.hdop() <= GPS_STATIC_MAX_HDOP;
    }

    // Stricter than hasGoodFix(): also requires the coordinate itself to
    // be changing fast enough to be real movement, not indoor multipath
    // jitter. positionSpeedMps is the caller-computed distance-since-last-
    // logged-point divided by elapsed time (can't be computed inside this
    // class - needs main.cpp's rtcLastLoggedLat/Lon and timing state).
    //
    // Note this is a single, position-only signal: HDOP/sat-count reflect
    // satellite geometry, not multipath, so a jittering indoor fix can
    // pass hasGoodFix() outright (see that comment) - GPS_MOVEMENT_MIN_
    // SPEED_MPS is what's actually relied on to reject it. Tune that
    // constant (in config.h) if jitter in your environment is fast enough
    // to still slip through, or if it's rejecting genuinely slow real
    // movement (e.g. walking pace).
    //
    // Use this - not hasGoodFix() - specifically for "should this fix
    // count as real movement and keep the device awake". Do NOT use it
    // for logging eligibility: a stationary anchor point (e.g. the very
    // first point of a session, taken before a car has pulled away) is
    // legitimate track data at zero speed and should still be logged.
    // Also requires the receiver's own speed-over-ground (RMC, Doppler-
    // derived) to agree: multipath jitter moves the computed POSITION by
    // metres between fixes, but barely affects the Doppler speed, which
    // stays near zero for a device sitting still. Position-derived speed
    // alone let any >= 5 m jump within 10 s count as movement.
    bool isConfirmedMoving(double positionSpeedMps) const {
        return hasGoodFix() && positionSpeedMps >= GPS_MOVEMENT_MIN_SPEED_MPS
            && _gps.speed.isValid() && _gps.speed.mps() >= GPS_MOVEMENT_MIN_SPEED_MPS;
    }

    TrackPoint currentAsTrackPoint(uint8_t batteryPct) const;

    // Receiver's own speed over ground (RMC, Doppler-derived), 0 if unknown.
    double speedMps() const { return _gps.speed.isValid() ? _gps.speed.mps() : 0.0; }
    double lat() const { return _gps.location.lat(); }
    double lon() const { return _gps.location.lng(); }
    uint32_t satellites() const { return _gps.satellites.value(); }

    // Full per-satellite table parsed from GSV sentences - see
    // SatelliteInfo above for why this is distinct from satellites().
    // Updated once per complete GSV cycle (all of that cycle's sentences
    // received), so callers never see a half-updated table mid-cycle.
    size_t satelliteListCount() const { return _satCount; }
    const SatelliteInfo* satelliteList() const { return _sats; }

    // distance in meters between last logged point and current fix
    double distanceFromMeters(double lat, double lon) const;

private:
    bool _clockSetThisBoot = false;
    bool _firstSyncPending = false;
    long _firstSyncStepS = 0;
    uint32_t _lastClockSetEpoch = 0;
    // TinyGPS++'s getter methods (lat(), lng(), value(), etc.) are not
    // const-qualified even though they're logically read-only (they just
    // happen to clear an internal "updated" flag as a side effect). Marked
    // mutable so GpsReader's own const accessors can call them.
    mutable TinyGPSPlus _gps;
    HardwareSerial _serial{1};
    // RMC's own time/status/date fields (both talker IDs this module may
    // use): the clock is set from these only - one sentence, so date and
    // time always belong together (see syncClockFromRmc()).
    struct RmcFields {
        TinyGPSCustom time, status, date;
    };
    RmcFields _rmc[2] = {
        {TinyGPSCustom(_gps, "GPRMC", 1), TinyGPSCustom(_gps, "GPRMC", 2), TinyGPSCustom(_gps, "GPRMC", 9)},
        {TinyGPSCustom(_gps, "GNRMC", 1), TinyGPSCustom(_gps, "GNRMC", 2), TinyGPSCustom(_gps, "GNRMC", 9)},
    };
    void syncClockFromRmc();

    // GSV ("satellites in view") parsing. TinyGPS++ doesn't parse GSV
    // itself - it only tracks aggregate fields (satellite COUNT, HDOP,
    // etc.) from GSA/GGA/RMC - so this is a small hand-rolled parser
    // alongside it, fed the same UART bytes in update().
    char _gsvLineBuf[96];
    size_t _gsvLineLen = 0;
    void feedGsvByte(char c);
    void parseGsvLine(char *line); // mutates line in place (comma -> '\0')

    // "Pending" table, filled in across a GSV cycle's multiple sentences
    // (message 1 of N, 2 of N, ...); copied into the public-facing _sats
    // table only once the last sentence in the cycle (msgNum == totalMsgs)
    // arrives, so satelliteList() never returns a half-built cycle.
    SatelliteInfo _pendingSats[MAX_TRACKED_SATS];
    size_t _pendingCount = 0;
    SatelliteInfo _sats[MAX_TRACKED_SATS];
    size_t _satCount = 0;

#ifdef GPS_DEBUG_RAW_NMEA
    // Line-buffers raw bytes off the GPS UART and echoes them to Serial,
    // prefixed "[GPS RAW]", for diagnosing "no fix" issues: if nothing
    // ever prints, no data is reaching the parser at all (baud/wiring/dead
    // module); if garbled text prints, it's a baud mismatch; if clean NMEA
    // sentences print but satellites/fix never come, it's a genuine
    // signal-acquisition issue (antenna/sky view/cold-start time). Enabled
    // via the GPS_DEBUG_RAW_NMEA build flag (see the -dev environment in
    // platformio.ini) - left off by default since it's noisy for normal use.
    char _debugLineBuf[128];
    size_t _debugLineLen = 0;
#endif
};

extern GpsReader gpsReader;
