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
// ~8.5 s of NMEA at 9600 baud - rides out the loop blocking during sync
// passes (see GpsReader::begin()).
#define GPS_UART_RX_BUFFER_BYTES 8192
// Extra host-name suffix the device web UI accepts besides IPs and common
// LAN suffixes (DNS-rebinding guard, see WebUi::requestAllowed), e.g.
// -D WEB_EXTRA_HOST_SUFFIX=\".corp.example.com\" for an office DNS zone.
#ifndef WEB_EXTRA_HOST_SUFFIX
#define WEB_EXTRA_HOST_SUFFIX ""
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
// 313, not 312: the register conversion rounds DOWN (312/62.5 = 4.99 ->
// 250 mg); 313 programs 5 LSB = 312.5 mg, one step below the 375 mg
// activity threshold. (Was 125 - inside the measured 60-110 mg noise band
// plus WiFi TX spikes >200 mg, so under vibration inactivity was never
// seen, activity never re-armed, and the device slept mid-trip.)
#define ADXL_INACT_THRESHOLD_MG 313   // inactivity threshold in mg (62.5mg/LSB steps)
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
// Two equal 100k resistors give ~2.5V. Awake, PowerManager::isCharging()
// reads the pin as ANALOG (>1.5V = power present); the deep-sleep plug-in
// wake (EXT1, level-triggered HIGH on the LP pad) uses the digital
// threshold (~2.475V), which 2.5V only just clears - but in practice it
// wakes reliably (verified 2026-10-14, also out of the Shut down state).
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
// Storage-almost-full warning (see SyncManager::storageStatus): distinct
// from the low-battery pattern - fewer, longer beeps, and far less often -
// and only in daytime (local time, clock must be set) so it never wakes
// anyone up at night.
#define STORAGE_BEEP_COUNT           3
#define STORAGE_BEEP_ON_MS           400
#define STORAGE_BEEP_OFF_MS          300
#define STORAGE_BEEP_INTERVAL_MS     (30UL * 60UL * 1000UL)
#define STORAGE_BEEP_DAY_START_HOUR  8    // local time, inclusive
#define STORAGE_BEEP_DAY_END_HOUR    21   // local time, exclusive
// "Almost full" = free space PLUS what could be reclaimed (uploaded tracks,
// when Settings -> Storage allows deleting them) is below this. At ~280 B
// per point that's about 1 h of driving left before recording stops.
#define STORAGE_WARN_BYTES           (160UL * 1024UL)
// Below this it's critical (recording stops soon / has stopped).
#define STORAGE_CRITICAL_BYTES       (64UL * 1024UL)


// ---------------------------------------------------------------------------
// Behaviour tuning
// ---------------------------------------------------------------------------
#define FLASH_PERSIST_INTERVAL_MS   (15UL * 60UL * 1000UL)  // 15 minutes
#define LOW_BATTERY_PERCENT         15                       // trigger flush+ maybe sleep
#define CRITICAL_BATTERY_PERCENT    5                        // force sleep, stop logging
// After a critical sleep, the boot reading must reach CRITICAL + this
// before a normal boot resumes (see setup()) - the cell reads a few %
// higher at rest than under WiFi+GPS load.
#define CRITICAL_RESUME_MARGIN_PERCENT 3
#define STATIONARY_TIMEOUT_MIN       7                        // minutes with no ADXL activity interrupt before sleep
// A motion wake is "unconfirmed" until WAKE_CONFIRM_ACTIVITY_COUNT
// accelerometer activity interrupts (the waking one included) or a
// GPS-confirmed movement. Until then, once the ADXL reports still again,
// the device goes back to sleep after UNCONFIRMED_WAKE_TIMEOUT_S instead of
// STATIONARY_TIMEOUT_MIN: a bag bumped at home kept it awake 7-10 min with
// the GPS searching indoors (4 such wakes cost ~10 % one evening). Moving
// continuously (no 3-s quiet, so the ADXL never reports still) keeps the
// normal timeout - walking to the car isn't cut short.
#define WAKE_CONFIRM_ACTIVITY_COUNT  3
// ...or this long in the ADXL's "moving" state without a 3-s quiet - a
// bike ride or a walk, whose continuous motion produces few new activity
// interrupts (the sensor only re-arms after a quiet period).
#define WAKE_CONFIRM_MOVING_MS       (60UL * 1000UL)
// While the ADXL reports moving, the stationary timeout doesn't run - for
// at most this long after its last activity interrupt (a vibrating surface
// must not keep the device awake forever). Before, continuous motion could
// put the device to sleep mid-ride while the GPS was still cold-starting.
#define ADXL_MOVING_HOLD_MAX_MS      (20UL * 60UL * 1000UL)
#define UNCONFIRMED_WAKE_TIMEOUT_S   120
#define SAFETY_WAKE_INTERVAL_SEC     (2UL * 60UL * 60UL)       // fallback timer wake even with zero motion, for a battery check (WiFi/GPS stay off - see main.cpp)
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
// How often, during a continuous-motion stretch that never goes back to
// sleep (e.g. a long drive), to get another WIFI_ACTIVE_WINDOW_MS chance
// at WiFi - see the periodic-recheck block in main.cpp's loop() for why
// this exists and why it works via a restart rather than resurrecting the
// radio in place. Equal to WIFI_ACTIVE_WINDOW_MS itself, so on battery
// during continuous motion WiFi ends up effectively always cycling: on
// for a window, a restart, on again - trading more frequent restart
// overhead (each one splits the GPX into a new file, ~3s cost) for
// noticeably more chances to pass within range of a known network.
#define WIFI_RECHECK_INTERVAL_MS      (5UL * 60UL * 1000UL)    // 5 minutes
// ...and only once the receiver has reported "stopped" (or lost its fix)
// for this long: WiFi is useless at driving speed, and each recheck is a
// restart plus a minute of scanning (~100 mA) - seven of them on a 42-min
// commute cost more than the GPS did. Parking at a site stops the car for
// good, a red light rarely lasts this long.
#define WIFI_RECHECK_STILL_MS         (45UL * 1000UL)
// After a drive (a point logged at >= WIFI_MOVED_MIN_SPEED_MPS since the
// last WiFi window), the first stop of WIFI_RECHECK_STILL_MS opens a window
// at once, without waiting for the recheck interval - arriving at home or
// the office should sync right away. At least WIFI_STOP_WINDOW_MIN_GAP_S
// apart, so stop-and-go traffic can't restart the device every minute.
#define WIFI_MOVED_MIN_SPEED_MPS      5.0f
#define WIFI_STOP_WINDOW_MIN_GAP_S    (3UL * 60UL)
// Standing still OUTDOORS (receiver says stopped, fix still trustworthy)
// has to last this long before it counts as parked - a red light or a
// junction is not an arrival (2026-10-12: a window opened mid-commute after
// a 45-s stop). Indoors (no trustworthy fix) WIFI_RECHECK_STILL_MS applies.
#define WIFI_STOPPED_OUTDOOR_MS       (120UL * 1000UL)
// "No trustworthy fix" only counts as parked indoors when the accelerometer
// has been quiet for WIFI_RECHECK_STILL_MS too, and - if the fix was lost at
// driving speed (tunnel, bad reception) - only after this long: on
// 2026-10-12/13 a lost or not-yet-acquired fix while driving restarted the
// device 6 times mid-drive, each restart costing GPS time.
#define WIFI_LOST_FIX_PARKED_MS       (5UL * 60UL * 1000UL)
// After a motion wake, the regular (non-arrival) window waits until the GPS
// has had a trustworthy fix, or this long: before the first fix the device
// doesn't know yet whether it is about to move (2026-10-12: a window opened
// at the car while the GPS was still cold-starting).
#define WIFI_NOFIX_FIRST_RECHECK_MS   (10UL * 60UL * 1000UL)
// After a window in which a backend answered, nothing was pending and no
// point was logged since boot, the next recheck waits this long instead:
// the device is parked somewhere with nothing to upload. A newly logged
// point pulls the recheck back to WIFI_RECHECK_INTERVAL_MS.
#define WIFI_RECHECK_IDLE_INTERVAL_MS (30UL * 60UL * 1000UL)
// CPU clock, always: GPS parsing, flash writes, the web server and WiFi
// (which needs at least 80 MHz on the C6) all run fine at 80 MHz, and the
// core draws roughly a third less than at 160.
#define CPU_MHZ_AWAKE                 80
// On battery, a window that is connected, found nothing to upload and has
// served no web request for this long is closed early - it would only
// keep the radio on for a web UI nobody is looking at. Any request
// (status poll from an open page) keeps it open another period.
#define WIFI_IDLE_CLOSE_MS            (90UL * 1000UL)
// The window opened on arrival after a drive is exempt from that idle
// close: WiFi (and the web UI) stays up until the device goes to sleep
// (STATIONARY_TIMEOUT_MIN after the last movement), capped at this long in
// case it keeps being moved around.
#define WIFI_ARRIVAL_WINDOW_MS        (10UL * 60UL * 1000UL)
#define GPS_FIX_TIMEOUT_MS           90000UL                  // give up waiting for fix
// GPS duty cycle without a fix (main.cpp loop, battery only): a device
// carried around indoors keeps the inactivity clock alive while the
// receiver searches in vain (~25 mA) - 85 min of that drained 25 % one
// evening. After GPS_NOFIX_SUSPEND_AFTER_MS without a fix the module is
// powered off for GPS_NOFIX_OFF_MS, then given GPS_NOFIX_RETRY_ON_MS to
// find one (outdoors a cold start takes ~35 s), and so on until it does.
// A tunnel or parking garage long enough to trigger this costs at most
// one off period of track.
// GPS duty cycle WITH a fix (battery only, main.cpp loop): the module
// draws ~25 mA whatever its fix rate (radio and tracking hardware run
// continuously; 1 Hz is also the slowest rate it offers), so the saving
// comes from switching it off between fixes. Its backup battery on VBAT
// keeps ephemeris and clock, so it is back with a fix ~1-2 s after power
// returns (hot start - seen in the log after planned restarts).
//  - moving: one cycle per GPS_DUTY_PERIOD_MS - on until GPS_DUTY_SETTLE_FIXES
//    good fixes (the first after power-on is the least accurate and is not
//    logged), the last one is evaluated for logging, then off for the rest
//    of the period (at least GPS_DUTY_MIN_OFF_MS).
//  - stopped (receiver says stopped, or ADXL still and slow): off for
//    GPS_DUTY_STOPPED_OFF_MS, ended early by accelerometer activity.
// No good fix: the module simply stays on (then the no-fix duty cycle
// below applies). On a charger: continuous 1 Hz tracking as before.
// GPS_DUTY_PERIOD_MS 0 disables the duty cycle.
// DISABLED: on this board the module does NOT hot-start after a real
// power cut - the first two drives with the duty cycle (2026-10-05/06)
// got a fix only minutes after each power-on (7-8 points per 40-minute
// drive, mostly at speed 0). The VBAT backup cell evidently doesn't hold
// the receiver's state (it is charged from VCC, which is off most of the
// time). Only power gaps of ~1 s (a restart) are bridged by the module's
// own capacitors. Re-enable only after "[GPS] First fix N s after power-on"
// in the log shows a few seconds after a gap of 10+ s (e.g. with a fresh
// backup cell, or with the module kept in standby via its ON/OFF pin
// instead of cutting VCC).
#define GPS_DUTY_PERIOD_MS           0UL
#define GPS_DUTY_SETTLE_FIXES        2
#define GPS_DUTY_MIN_OFF_MS          1500UL
#define GPS_DUTY_STOPPED_OFF_MS      (30UL * 1000UL)
// Light sleep while the GPS module is off (duty-cycle and no-fix off
// periods) on battery with WiFi off: the CPU sleeps in slices of
// LIGHT_SLEEP_SLICE_MS (timer wake) instead of idling at ~20 mA, and each
// slice end polls the accelerometer's latched activity flag. With the GPS
// off there is no UART traffic that light sleep could garble. Set to 0 to
// disable.
// DISABLED: after a light sleep the ADC read the charge-detect pin and the
// battery voltage too high (2026-10-06: battery +12-15 %, "charging"
// while on battery), which triggered 27 "charger appeared" restarts in
// 30 minutes of driving. Needs a bench session on USB serial before it
// can come back (probably re-initialising the two analog pins after wake).
#define LIGHT_SLEEP_ENABLED          0
#define LIGHT_SLEEP_SLICE_MS         1000UL
#define GPS_NOFIX_SUSPEND_AFTER_MS   (10UL * 60UL * 1000UL)
#define GPS_NOFIX_OFF_MS             (5UL * 60UL * 1000UL)
#define GPS_NOFIX_RETRY_ON_MS        (2UL * 60UL * 1000UL)
#define TRACKPOINT_MIN_INTERVAL_MS   3000UL                   // don't log faster than this
#define TRACKPOINT_MIN_DISTANCE_M    5.0f                     // or unless moved this far

// Gates on GpsReader::hasGoodFix() - see its comment in gps_reader.h for
// why hasFix() alone isn't enough to trust a GPS-measured displacement as
// real movement. HDOP: lower is better; <=2 excellent, <=5 good, >10
// poor/unusable. 3.0 errs strict on purpose - a false negative here just
// leaves the ADXL accelerometer to decide sleep on its own (harmless);
// a false positive is the indoor-never-sleeps bug this exists to fix.
#define GPS_MOVEMENT_MAX_HDOP        3.0f
#define GPS_MOVEMENT_MIN_SATS        5        // satellites actually USED in the fix (GSA), not just in view
// Indoor-fix gate (GpsReader::hasTrustedFix()), on top of the above: below
// GPS_TRUSTED_SPEED_MPS (8 km/h - slow cycling still counts as moving) a fix
// is only trusted with open-sky quality
// (>= GPS_STATIC_MIN_SATS satellites and HDOP <= GPS_STATIC_MAX_HDOP).
// Indoors the receiver keeps a "good" fix (5-9 satellites, HDOP 1.5-3)
// that wanders up to 300 m with FAKE speeds of 1-2 m/s - which defeats the
// stop/go filter and kept the device "moving" around the house for 45 min
// (2026-10-08; none of those points reached 8 km/h). On the recorded tracks
// this keeps 5962 of 5988 driving points (the rest are stops at traffic
// lights) and rejects all 50 indoor points of that evening; night-time
// strays that pass are 1-2-point trips, deleted by MIN_SESSION_POINTS.
#define GPS_TRUSTED_SPEED_MPS        (8.0f / 3.6f)
#define GPS_STATIC_MIN_SATS          6   // satellites USED in the fix (GGA), like GPS_MOVEMENT_MIN_SATS
#define GPS_STATIC_MAX_HDOP          1.2f // the real indoor filter: indoor fixes never got below 1.5
// A trip with fewer points than this is noise (a bump at night that got a
// fix, the device carried around indoors) and is deleted when it ends.
#define MIN_SESSION_POINTS           5

// Additional gate on top of hasGoodFix(), used ONLY for the sleep/keep-
// awake decision (GpsReader::isConfirmedMoving()) - NOT for logging.
// HDOP/satellite count alone doesn't reliably catch indoor multipath on a
// budget single-constellation receiver like the ATGM336H on this board:
// multipath corrupts the pseudorange measurements themselves, not the
// satellite geometry HDOP is computed from, so a jittering indoor fix can
// still report perfectly decent HDOP/sat-count. This is checked against
// positionSpeedMps - distance-since-last-logged-point divided by elapsed
// time, i.e. how fast the reported coordinate is actually changing - so
// it's the single line of defense against fast-jittering indoor fixes:
// raise it if jitter in your environment routinely computes out to more
// than 0.5 m/s (1.8 km/h) between points, at the cost of taking longer to
// confirm genuinely slow real movement (e.g. walking pace); lower it if
// slow real movement is being missed and jitter isn't an issue for you.
#define GPS_MOVEMENT_MIN_SPEED_MPS   0.5f
// Stillness filter (main.cpp loop): while the ADXL reports inactivity (no
// movement above the activity threshold since its last 3-s quiet period)
// AND the receiver's own speed is below this, no point is logged. Indoors
// a lying-still receiver keeps producing "good" fixes that jump 5-10 m
// with a fake 0.4-1.9 m/s speed; walking never looks still to the ADXL
// (no 3 s of quiet), and driving is logged on speed alone.
#define GPS_STILL_MAX_SPEED_MPS      2.0f
// GPS stop/go hysteresis (main.cpp loop), on top of the ADXL filter: the
// ADXL can't help when the device is carried around indoors - the GPS
// keeps a "good" fix there that drifts 5-100 m with a Doppler speed of
// ~0 (the scribble at the end of a trip that ends at home). Once
// GPS_STOP_FIXES consecutive fixes report less than GPS_STOP_MAX_SPEED_MPS
// the receiver itself says "not moving": until GPS_GO_FIXES consecutive
// fixes report at least GPS_GO_MIN_SPEED_MPS again (walking/driving), a
// point is only logged when it is TRACKPOINT_STOPPED_MIN_DISTANCE_M from
// the last logged one. Stops at traffic lights get quieter too; a very
// slow stroll is thinned to one point per 50 m.
#define GPS_STOP_MAX_SPEED_MPS       0.5f
#define GPS_STOP_FIXES               3
#define GPS_GO_MIN_SPEED_MPS         1.0f
#define GPS_GO_FIXES                 2
#define TRACKPOINT_STOPPED_MIN_DISTANCE_M 50.0f
// ...and that far position must hold for this many consecutive good fixes
// before it is logged: a single indoor drift spike 100+ m away is gone on
// the next fix, a real move stays.
#define GPS_STOPPED_FAR_FIXES        3
// How often the ADXL interrupt-source register is polled for its
// activity/inactivity flags (inactivity has no wired interrupt).
#define ADXL_STATE_POLL_MS           250UL
// No ADXL activity at all this long after configuring it = lying still.
#define ADXL_ASSUME_STILL_MS         5000UL
#define WIFI_SYNC_CHECK_INTERVAL_MS  (30UL * 1000UL)
// WiFi association retry/alternation (see WifiConnector). A single failed
// association used to never be retried - while charging, wifiActive stays
// true forever, so the device sat awake with WiFi dead until a reflash
// forced a fresh boot. Now each attempt gets WIFI_SLOT_TRY_MS, after which
// the next configured network is tried (the same one again if only one is
// set up). An attempt the driver has already declared hopeless (network
// not in range, wrong password) moves on after WIFI_SLOT_FAST_FAIL_MS, so
// switching between home and office costs seconds, not a full timeout.
#define WIFI_SLOT_TRY_MS             (20UL * 1000UL)
#define WIFI_SLOT_FAST_FAIL_MS       (6UL * 1000UL)
// Both saved networks reported "not found" in a row: wait this long before
// the next scan round (scanning every 6 s cost real battery on drives
// with no known WiFi around), and on battery close the WiFi window after
// WIFI_NONE_IN_RANGE_CLOSE_MS of that instead of the full 5 minutes.
#define WIFI_SCAN_BACKOFF_MS         (30UL * 1000UL)
#define WIFI_NONE_IN_RANGE_CLOSE_MS  (45UL * 1000UL)
// Fresh boot with saved networks but none reachable: keep trying this long,
// then open the setup portal (AP mode). Covers both slots' fast-fail time.
#define WIFI_BOOT_PORTAL_WAIT_MS     (30UL * 1000UL)
// Still unconnected after this long while charging: if the device is also
// stationary (no motion for WIFI_PORTAL_STATIONARY_S) and the setup portal
// hasn't been offered during this offline streak yet, open it - that's how
// a network gets added at a new location, now that stored networks make
// "is WiFi configured" permanently true. Otherwise (moving, e.g. charging
// in a car) fall back to the proven restart-into-boot recovery (planned
// restart, session/inactivity state preserved). Charging-only: on battery
// the active window closes after WIFI_ACTIVE_WINDOW_MS anyway, and the
// portal blocks the main loop (no GPS logging) for up to 3 minutes.
#define WIFI_STUCK_RESTART_MS        (3UL * 60UL * 1000UL)
// 10 min, not 2: a wait at a red light or in a jam on a car charger must
// not count as "stationary" (the portal blocks GPS logging for 3 min).
#define WIFI_PORTAL_STATIONARY_S     600
// No network saved at all, on the charger: how often the setup portal is
// offered again after one timed out unattended.
#define WIFI_UNCONFIGURED_PORTAL_RETRY_MS (10UL * 60UL * 1000UL)
// A charger state change must hold this long before the plug-in restart /
// unplug grace window react to it (flaky Qi pad or car USB).
#define CHARGE_DEBOUNCE_MS           5000UL
// "Charger appeared mid-cycle" restart (main.cpp): only after the charger
// has been present this long without a dropout, and at most once per
// CHARGER_RESTART_MIN_INTERVAL_S - a flickering car Qi pad or a bad
// reading must not turn into a restart loop (each restart cuts the GPS).
#define CHARGER_RESTART_STABLE_MS    (30UL * 1000UL)
#define CHARGER_RESTART_MIN_INTERVAL_S (10UL * 60UL)
// With WiFi up, an optional restart waits at most this long for a failed
// flush to succeed before restarting anyway (the RAM points would be lost
// at the next sleep regardless).
#define RESTART_HOLD_FOR_UNSAVED_MS  (30UL * 60UL * 1000UL)
// Once-a-minute one-line state dump on serial. Exists because a healthy
// idle device prints NOTHING for minutes at a time, which is
// indistinguishable from a wedged one when diagnosing over USB - with
// this, silence for >1 min means the firmware is genuinely stuck.
#define HEARTBEAT_INTERVAL_MS        (60UL * 1000UL)
// Consecutive failed probes/uploads before sync pauses, and how long the
// pause lasts (also lifted by "Sync now", a WiFi reconnect, or a wake).
// Unsynced files stay pending on flash - nothing is lost by backing off.
#define SYNC_MAX_UPLOAD_FAILURES      10
#define SYNC_GIVEUP_RETRY_MS          (10UL * 60UL * 1000UL)
// Backend probe (GET /healthz). On arrival at the office the LAN path from
// a freshly associated client to the backend took 2-10 min to come alive
// (every connect failed within 3 s while the link itself was fine, then
// suddenly worked) - so: a longer connect timeout, a quicker re-probe
// after a failed round, and a shorter pause when no backend answered at
// all (the 10-min upload give-up pause made the device miss the moment
// the path healed).
#define SYNC_PROBE_CONNECT_MS         5000
#define SYNC_PROBE_RETRY_MS           (15UL * 1000UL)
#define SYNC_NO_BACKEND_RETRY_MS      (2UL * 60UL * 1000UL)
// A single file failing this many times in a row (non-4xx) is parked like
// a rejected one until it changes or "Sync now".
#define SYNC_FILE_MAX_FAILURES        3
// Rejected/parked files are retried after this long anyway.
#define SYNC_REJECT_RETRY_MS          (30UL * 60UL * 1000UL)
// Last-resort space reclaim (SyncManager::reclaimSpace, before every track
// flush, with or without WiFi): tracks already on a backend are deleted -
// oldest first - only when free space would drop below what the flush
// needs plus this margin (ledger, config and debug-log writes need room
// too). The device thus keeps as much history as fits. Disabled entirely
// by Settings -> Storage (DeviceConfig::reclaimSynced). Trade-off with two
// backends: a file reclaimed after reaching one backend never reaches the
// other - only when the flash is full, and never unsynced data.
#define SYNC_FS_MIN_FREE_BYTES        (64UL * 1024UL)
#define RAM_TRACKPOINT_CAPACITY       2000  // ring buffer size in RAM (~44 bytes/pt = ~88KB)
// GPX text per trackpoint on flash (measured ~270 B) - sizing estimate for
// space reclaim ahead of a flush.
#define GPX_BYTES_PER_POINT           280
// Points per committed chunk when flushing (partial progress on low space).
#define FLUSH_CHUNK_POINTS            200
// After a failed flush (flash full), how long before a full buffer retries.
#define FLUSH_RETRY_BACKOFF_MS        (60UL * 1000UL)

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
#define FS_DEBUG_LOG_PATH     "/debug.log"       // see DebugLog
#define FS_DEBUG_LOG_OLD_PATH "/debug.old.log"   // previous file after rotation

// Debug log (see debug_log.h): RAM buffer between flash saves, save
// interval, and the size at which the file rotates (so at most ~2x this
// is ever on flash - small next to the track reserve).
#define DEBUG_LOG_BUF_BYTES          8192 // a restart flushes/reclaims/logs in one burst - 6 KB dropped lines
#define DEBUG_LOG_SAVE_INTERVAL_MS   (30UL * 1000UL)
#define DEBUG_LOG_FILE_MAX_BYTES     (48UL * 1024UL)
// Below this much free flash the debug log stops writing and gives back its
// older half (tracks win). Must stay well below SYNC_FS_MIN_FREE_BYTES:
// at the same 64 KB, the track cleanup (which keeps exactly that much free)
// regularly pushed the log over its limit, and it lost its history - the
// log of a whole day was gone the next morning (2026-10-12).
#define DEBUG_LOG_MIN_FREE_BYTES     (32UL * 1024UL)

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
    // Up to two backends (e.g. home HA and office HA - both on 192.168.8.x
    // here, so the device cannot tell the sites apart by network). At sync
    // time the device probes them in order and uses the first one that
    // answers /healthz - see SyncManager::resolveBackend(). The sync ledger
    // is kept PER BACKEND, so each site independently receives the full
    // track history regardless of where a session was first uploaded.
    char backendHost[64];   // primary backend, e.g. "192.168.8.233" (set via portal or web UI)
    uint16_t backendPort;   // REST API port
    char backendHost2[64];  // optional second backend ("" = none; set via web UI)
    uint16_t backendPort2;
    // Up to two WiFi networks, same idea as the backends (see WifiConnector):
    // the device tries the one that connected last, then alternates. Kept
    // here rather than only in the ESP-IDF's NVS, which holds ONE network.
    // The primary is filled by the captive portal (or migrated from NVS on
    // first boot after upgrading); both are editable in the web UI.
    // Sizes: 802.11 SSID max 32 bytes, WPA2 passphrase max 63 (+NUL).
    char wifiSsid[33];
    char wifiPass[65];
    char wifiSsid2[33];     // "" = no second network
    char wifiPass2[65];
    // Set once the one-time import of the NVS-stored network has run
    // (ConfigManager::migrateLegacyWifi) - so a later "Reset WiFi" that
    // empties both slots can never re-import a stale NVS copy.
    bool wifiMigrated;
    bool debugLog;          // record the debug log to flash (see DebugLog)
    // Settings -> Storage: may the device delete its oldest tracks that are
    // already on a backend when new points wouldn't fit otherwise? Last
    // resort only (see SyncManager::reclaimSpace); off = never deletes.
    bool reclaimSynced;
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
