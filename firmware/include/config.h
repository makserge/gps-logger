#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Hardware pins (override via build_flags in platformio.ini)
//
// XIAO ESP32-C6 only exposes 11 GPIOs on its header, labeled D0-D10:
//   D0=GPIO0  D1=GPIO1  D2=GPIO2  D3=GPIO21 D4=GPIO22 D5=GPIO23
//   D6=GPIO16 D7=GPIO17 D8=GPIO19 D9=GPIO20 D10=GPIO18
// Any pin assignment below MUST come from that list. GPIO3/GPIO4/GPIO6/
// GPIO7/GPIO9/GPIO10/GPIO14 etc. are either not routed to the header at
// all, or (GPIO3/GPIO14 specifically) reserved internally for the board's
// antenna RF switch - don't reuse them for peripherals.
// ---------------------------------------------------------------------------
#ifndef GPS_RX_PIN
#define GPS_RX_PIN 20     // D9 - ESP32 RX <- GPS TX
#endif
#ifndef GPS_TX_PIN
#define GPS_TX_PIN 18     // D10 - ESP32 TX -> GPS RX
#endif
#ifndef GPS_BAUD
#define GPS_BAUD 9600
#endif
// Hardware power control for the GPS module, via its dedicated ON/OFF
// pin (pin 5). This is superseded by the wiring actually used: an AO3401
// P-channel MOSFET (Q1) as a high-side switch gates the module's main
// VCC instead - Source->3.3V, Drain->GPS VCC, Gate->GPS_POWER_PIN through
// a 150R series resistor (R1), with a 10K pull-up (R2) from the gate node
// to 3.3V so the switch defaults OFF if the GPIO is left floating (e.g.
// during boot before pinMode() runs). Pin 5/ON-OFF is NOT used in this
// build - VCC itself is what gets switched.
//
// The module's RTC/SRAM backup power comes from a separate pin (VBAT,
// pin 6, spec'd 1.5-3.6V per the ATGM336H-5N31 datasheet) - this
// breakout has its own backup battery wired to VBAT independently of
// the switched main VCC rail, so that domain should stay powered even
// with main VCC fully cut, same as it would under an ON/OFF shutoff.
// Per the datasheet's TTFF spec: cold start (backup power actually
// lost) is <=35s, hot start (backup power intact, expected here) is
// <=1s - worth confirming which one you're actually seeing in practice,
// since a full VCC cut is a more thorough power-down than the chip's own
// ON/OFF standby, and GPS_POWER_STABILIZE_MS below may need to be longer
// too (the module's regulator and digital core have to restart from a
// full power-off, not just exit an internal standby state).
#ifndef GPS_POWER_PIN
#define GPS_POWER_PIN 23 // D5
#endif
#ifndef GPS_POWER_ON_LEVEL
// LOW = on: GPS_POWER_PIN drives the AO3401's gate (Q1, see above). A
// P-FET's gate must go LOW relative to its source to turn ON - inverted
// from what driving the module's ON/OFF pin directly would need (that
// pin is active-low shutoff, so HIGH = on). If you ever go back to
// wiring GPS_POWER_PIN straight to pin 5 instead of this MOSFET gate,
// flip this back to HIGH.
#define GPS_POWER_ON_LEVEL LOW
#endif
// Time to let the module come back up before UART is expected to
// respond, distinct from time-to-fix (TTFF). Per the datasheet, hot-start
// TTFF (backup power intact, which applies here) is <=1s and cold-start
// is <=35s - that's the time to acquire a fix, not the time until UART
// output resumes, which should be quicker. Whatever polls for a fix
// downstream (hasFix()/update()) still needs to allow for that <=1s hot
// TTFF on top of this delay. Tune GPS_POWER_STABILIZE_MS itself if the
// first NMEA sentences after a wake look garbled/truncated.
#ifndef GPS_POWER_STABILIZE_MS
#define GPS_POWER_STABILIZE_MS 200
#endif
#ifndef BATTERY_ADC_PIN
#define BATTERY_ADC_PIN 1 // D1/A1 - resistor divider to battery+ (ADC1, GPIO0-6 only on this chip)
#endif

// ADXL345 accelerometer (I2C) - used for motion-interrupt wake instead of
// periodic GPS-coordinate polling. INT1 is wired to a GPIO and used both as
// a live interrupt while awake and as a deep-sleep GPIO wakeup source.
// SDA/SCL are on D7/D8 (same side of the board as the GPS module, D9/D10,
// for short wire runs). INT1 CANNOT move there too, though: ESP32-C6 deep-
// sleep GPIO wakeup only works on its 8 LP_IO pins (GPIO0-GPIO7), and D7/D8/
// D9/D10 (GPIO17/19/20/18) aren't in that range - moving INT1 off D0/D1/D2
// breaks motion-wake at the first sleep cycle with "invalid deep sleep
// wakeup IO". D2 is the only LP_IO pin near the D6-D10 side, so INT1 stays
// there; it's the one wire on this sensor that can't be shortened the same way.
#ifndef ADXL_SDA_PIN
#define ADXL_SDA_PIN 17    // D7
#endif
#ifndef ADXL_SCL_PIN
#define ADXL_SCL_PIN 19    // D8
#endif
#ifndef ADXL_INT_PIN
#define ADXL_INT_PIN 2     // D2 - must stay in GPIO0-7 (LP_IO) for deep-sleep wake to work
#endif
#ifndef ADXL_I2C_ADDR
#define ADXL_I2C_ADDR 0x53 // 0x53 with SDO/ALT-ADDR tied low, 0x1D if tied high
#endif
// Activity threshold in mg, quantized to the register's 62.5mg/LSB steps
// (values round DOWN: 300 programmed 250mg). This sets how much movement
// counts as "moved": 250mg needs a firm shake/bump, 125mg (2 LSB) catches
// being picked up and carried gently. Lower = more sensitive = wakes/stays
// awake on softer motion, at the cost of false triggers. MEASURED on this
// hardware (raw-sample diagnostic in main.cpp, 2026-09-21): the sensor's
// noise floor on a completely still board is 60-110mg sample-to-sample,
// continuously - ~20x the ADXL345's spec (<5mg), i.e. electrical noise
// (wiring/supply) or a counterfeit sensor, NOT real vibration. Since the
// activity engine compares samples against a captured reference, opposite
// noise excursions stack: 125 and 187 fire constantly on a still board,
// 250 is reliably quiet. 250 is therefore the floor on this wiring. To
// enable gentler thresholds, fix the noise first (100nF+10uF right at the
// ADXL's VS pin, short/soldered supply+ground leads, genuine sensor) and
// re-measure with the "peak sample delta" diagnostic: the threshold needs
// ~2x headroom over the observed still-board peaks. Set at 375 (6 LSB,
// exact): 250 was already quiet, this adds 50% margin against noise
// outliers at the cost of needing a firmer bump to register motion. NOTE
// the 62.5mg quantization rounds DOWN - e.g. 312 would silently program
// 250; valid steps are 250, 312.5, 375, 437.5, 500...
#define ADXL_ACT_THRESHOLD_MG   375
// Inactivity detection exists only to re-arm AC-coupled activity detection
// with a fresh gravity reference after each motion episode (LINK mode - see
// MotionSensor::begin()): all samples must stay below this threshold for
// ADXL_INACT_TIME_S before the next activity can trigger. Keep it equal to
// (or one 62.5mg step below) ADXL_ACT_THRESHOLD_MG - NOT much lower:
// ambient vibration sitting between the two thresholds would block re-arm
// indefinitely, leaving a sleeping device unable to motion-wake until it
// gets a fully quiet window.
#define ADXL_INACT_THRESHOLD_MG 125   // inactivity threshold in mg (62.5mg/LSB steps)
#define ADXL_INACT_TIME_S       3     // seconds below threshold before "inactive" (1s/LSB)

// XIAO ESP32-C6 has a built-in battery power management chip: solder the
// LiPo directly to the board's onboard BAT+ / BAT- pads, and it charges
// automatically whenever USB-C (or the 5V pin) is powered - no external
// TP4056 or similar charge IC is needed.
//
// The board does not expose a CHRG/STAT status line, though, so "charging"
// here is inferred instead from whether 5V is present at all: the XIAO's
// 5V pin carries USB-C VBUS when powered and reads 0V when running on
// battery alone. Feed it through a resistor divider into this GPIO.
// IMPORTANT sizing: two equal 100k resistors give ~2.5V, which is almost
// exactly the C6's digital VIH (0.75*3.3V = 2.475V) - zero noise margin.
// Awake detection works anyway because PowerManager::isCharging() reads
// the pin as ANALOG (>1.5V = power present), but the deep-sleep plug-in
// wake (EXT1, level-triggered HIGH on the LP pad) uses the DIGITAL
// threshold and is unreliable at 2.5V. Use ~100k top / ~180k bottom
// instead (~3.2V at 5V-present, still below the pad's 3.3V rating) for a
// solid margin on both paths.
#ifndef CHARGE_STATUS_PIN
#define CHARGE_STATUS_PIN 0 // D0 - HIGH via divider off the 5V pin when USB-C/Qi power is present
#endif

// Active 3.3V buzzer for low-battery audible alert.
#ifndef BUZZER_PIN
#define BUZZER_PIN 22 // D4
#endif
#define LOW_BATTERY_BEEP_COUNT     5
#define LOW_BATTERY_BEEP_ON_MS     120
#define LOW_BATTERY_BEEP_OFF_MS    120
#define LOW_BATTERY_BEEP_INTERVAL_MS (60UL * 1000UL) // repeat pattern once a minute


// ---------------------------------------------------------------------------
// Behaviour tuning
// ---------------------------------------------------------------------------
#define FLASH_PERSIST_INTERVAL_MS   (15UL * 60UL * 1000UL)  // 15 minutes
#define LOW_BATTERY_PERCENT         15                       // trigger flush+ maybe sleep
#define CRITICAL_BATTERY_PERCENT    5                        // force sleep, stop logging
#define STATIONARY_TIMEOUT_MIN       15                       // minutes with no ADXL activity interrupt before sleep
#define SAFETY_WAKE_INTERVAL_SEC     (60UL * 60UL)             // fallback timer wake even with zero motion, for a battery check (WiFi/GPS stay off - see main.cpp)
// Much longer wake interval used specifically when the battery is already
// critical at boot (see main.cpp) - the normal hourly safety wake still
// briefly powers up WiFi/GPS every cycle before the critical check can run
// (they're brought up early in setup(), for OTA-recoverability reasons),
// which burns real current repeatedly on an already-critical cell. This
// interval is used instead once critical, so the device mostly just sleeps
// and checks back in far less often, still allowing it to notice once
// charging resumes.
#define CRITICAL_BATTERY_SLEEP_INTERVAL_SEC (6UL * 60UL * 60UL) // 6 hours
// WiFi/the web server are gated by motion the same way GPS is (see
// main.cpp's gpsActive/wifiActive), but on a bounded timer rather than for
// the whole active period: once a motion wake brings WiFi up, it's forced
// back off after this long regardless of whether motion (and GPS logging)
// continues, since it's meant as a periodic sync/OTA opportunity rather
// than something that needs to stay on continuously. A safety-timer-only
// wake (no motion) never brings WiFi up at all - see main.cpp.
#define WIFI_ACTIVE_WINDOW_MS         (5UL * 60UL * 1000UL)    // 5 minutes
#define GPS_FIX_TIMEOUT_MS           90000UL                  // give up waiting for fix
#define TRACKPOINT_MIN_INTERVAL_MS   3000UL                   // don't log faster than this
#define TRACKPOINT_MIN_DISTANCE_M    5.0f                     // or unless moved this far
#define WIFI_SYNC_CHECK_INTERVAL_MS  (30UL * 1000UL)
// Consecutive failed REST/GPX uploads before giving
// up until the next boot/wake. Unsynced files stay pending on flash and are
// retried automatically next wake cycle - nothing is lost by backing off.
#define SYNC_MAX_UPLOAD_FAILURES      10
#define RAM_TRACKPOINT_CAPACITY       2000  // ring buffer size in RAM (~44 bytes/pt = ~88KB)

// Battery divider calibration (adjust to your resistor divider / board)
#define BATTERY_ADC_MAX_MV   4200.0f   // voltage at ADC pin representing full battery
#define BATTERY_ADC_MIN_MV   3000.0f   // empty
// Nominal ratio for a 100k/100k divider is 2.0, but ESP32's ADC tends to
// under-read through source impedances much above ~10kOhm (its sample-and-
// hold capacitor can't fully charge within the sampling window), so the
// *effective* ratio needed to get an accurate reading is usually a bit
// higher than the resistors' math alone suggests. To calibrate: compare a
// multimeter reading of the actual battery voltage against what the device
// reports (readBatteryVoltage()), then set this to
// current_ratio * (actual_mV / reported_mV). Pre-set below from a reported
// 4116mV vs an actual 4196mV (2.0 * 4196/4116 ~= 2.039) - re-verify against
// your own multimeter and adjust further if needed, ideally at more than
// one charge level in case the error isn't perfectly linear.
#define BATTERY_DIVIDER_RATIO 2.0324f

// ---------------------------------------------------------------------------
// Filesystem layout (LittleFS)
// ---------------------------------------------------------------------------
// The board's clock is only ever set from GPS NMEA data (no RTC chip), so
// time(nullptr) reads a small epoch (effectively 1970-01-01) until the
// first fix ever arrives. Any epoch below this floor is treated as "not
// synced yet" - used to gate session/file creation (TrackStore) and status
// reporting (web UI, MQTT) so nothing is ever written or displayed with a
// bogus pre-sync timestamp.
#define PLAUSIBLE_EPOCH_FLOOR ((time_t)1577836800) // 2020-01-01 UTC

#define FS_CONFIG_PATH        "/config.json"
#define FS_SESSIONS_DIR        "/tracks"          // /tracks/YYYY-MM-DD/<sessionId>.gpx
#define FS_PENDING_DIR          "/pending"         // sessions not yet synced (symlink-ish index)
#define FS_SYNC_STATE_PATH    "/sync_state.json"

// ---------------------------------------------------------------------------
// Trackpoint - compact in-RAM representation
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct TrackPoint {
    uint32_t epoch;      // unix timestamp (UTC)
    int32_t  lat_e7;     // latitude  * 1e7
    int32_t  lon_e7;     // longitude * 1e7
    int16_t  alt_dm;     // altitude in decimeters
    uint16_t speed_cms;  // speed in cm/s
    uint16_t course_dd;  // course in 0.1 deg units (0-3599)
    uint8_t  sats;       // satellites used
    uint8_t  hdop_x10;   // HDOP * 10 (capped at 255)
    uint8_t  battery_pct;// battery % at time of fix
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Device config persisted to flash (set via WiFiManager captive portal)
// ---------------------------------------------------------------------------
struct DeviceConfig {
    char backendHost[64];   // e.g. "homeassistant.local" or IP
    uint16_t backendPort;   // REST API port
    char deviceId[24];      // unique id, derived from MAC if empty
    bool otaEnabled;
    // POSIX TZ string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3" for Central
    // Europe), set via the web UI. Applied with setenv("TZ", ...)+tzset()
    // at boot and whenever changed, so localtime_r() automatically handles
    // DST transitions using the C library's own rules - no manual
    // twice-a-year offset flipping needed. Affects the "Board time" shown
    // in the web UI/MQTT status payload AND session date-folders/filenames
    // (so trips are grouped/named by local calendar day, not UTC day).
    // GPX <trkpt><time> values inside files stay UTC always, per spec, for
    // interchange correctness - this never touches those. Defaults to
    // "UTC0" (no offset, no DST).
    char tzPosix[48];
};
