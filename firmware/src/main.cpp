#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <LittleFS.h>
#include "config.h"
#include "debug_log.h"
#include "config_manager.h"
#include "track_store.h"
#include "gps_reader.h"
#include "power_manager.h"
#include "motion_sensor.h"
#include "sync_manager.h"
#include "wifi_connector.h"
#include "web_server_ui.h"
#include "buzzer.h"
#include "lifecycle.h"
#ifdef DISABLE_BROWNOUT_DETECTOR
#include "esp_private/brownout.h"
#endif

// -----------------------------------------------------------------------
// State that survives deep sleep AND ESP.restart()
//
// NOT RTC_DATA_ATTR: on the ESP32-C6 the bootloader reloads .rtc.data from
// the firmware image on every reset except a deep-sleep wake, so a planned
// restart silently reset all of it (verified: "[Boot #1]" after every
// restart) - the continue-the-run and once-per-streak logic below never
// took effect. RTC_NOINIT_ATTR memory is left alone by the bootloader and
// startup code; it only holds garbage after a power loss, which
// rtcStateInit() detects (reset reason + magic) and zeroes.
// -----------------------------------------------------------------------
// Bump the low byte whenever the struct layout changes, so an OTA to a
// different layout starts clean instead of misreading old bytes.
static constexpr uint32_t RTC_STATE_MAGIC = 0x47504C00u | 0x08u;
struct RtcState {
    uint32_t magic;
    double   lastLoggedLat;
    double   lastLoggedLon;
    bool     hasLastPoint;
    uint32_t lastMotionEpoch;   // updated on every ADXL activity interrupt
    uint32_t sessionStartEpoch;
    uint32_t bootCount;
    // Set immediately before a PLANNED ESP.restart() (WiFi-window refresh,
    // charger-appeared handler, WiFi recovery) so the next setup() can tell
    // it apart from a genuine cold boot - see continuedRun in setup().
    bool     plannedRestart;
    // WiFi offline-streak recovery (see the WifiConnector block in loop()):
    // each step runs at most once per streak of not reaching any known
    // network; both reset as soon as a connection succeeds.
    bool     openPortalOnBoot;
    bool     wifiPortalOffered;
    bool     wifiRecoveryRestartDone;
    // Epoch of the last logged point. Position-derived speed must use the
    // time since THAT point: millis() restarts at every boot, so after a
    // restart/wake "distance to the old point / uptime" came out hugely
    // inflated and a still device voted itself awake forever.
    uint32_t lastPointEpoch;
    // Slept because the battery was critical: the next boot's single
    // unloaded reading must clear a margin before resuming (hysteresis
    // against wake/sleep cycling on a nearly empty cell).
    bool     criticalSleep;
    // The ADXL failed to configure last cycle, so sleep had no motion wake:
    // timer wakes must then do the GPS/WiFi work motion wakes normally do.
    bool     motionSensorFailed;
    // GPS state at a planned restart - restored instead of forcing it on.
    bool     gpsWasActive;
    // Clock at deep-sleep entry: the first GPS sync after the wake logs how
    // far the RC slow clock drifted meanwhile (see README, clock drift).
    // When the clock was last set from GPS or NTP: the first GPS sync of
    // the next boot measures the drift accumulated since then.
    uint32_t lastClockSyncEpoch;
    // Last "charger appeared mid-cycle" restart - rate limit across restarts.
    uint32_t chargerRestartEpoch;
    // Start of the last WiFi window - spacing for stop-triggered windows.
    uint32_t wifiWindowEpoch;
    // The restart about to happen opens the arrival window (see
    // WIFI_ARRIVAL_WINDOW_MS).
    bool arrivalWindow;
};
RTC_NOINIT_ATTR static RtcState rtc;

static double   &rtcLastLoggedLat = rtc.lastLoggedLat;
static double   &rtcLastLoggedLon = rtc.lastLoggedLon;
static bool     &rtcHasLastPoint = rtc.hasLastPoint;
static uint32_t &rtcLastMotionEpoch = rtc.lastMotionEpoch;
static uint32_t &rtcSessionStartEpoch = rtc.sessionStartEpoch;
static uint32_t &rtcBootCount = rtc.bootCount;
static bool     &rtcPlannedRestart = rtc.plannedRestart;
static bool     &rtcOpenPortalOnBoot = rtc.openPortalOnBoot;
static bool     &rtcWifiPortalOffered = rtc.wifiPortalOffered;
static bool     &rtcWifiRecoveryRestartDone = rtc.wifiRecoveryRestartDone;
static uint32_t &rtcLastPointEpoch = rtc.lastPointEpoch;
static bool     &rtcCriticalSleep = rtc.criticalSleep;
static bool     &rtcMotionSensorFailed = rtc.motionSensorFailed;
static bool     &rtcGpsWasActive = rtc.gpsWasActive;
static uint32_t &rtcLastClockSyncEpoch = rtc.lastClockSyncEpoch;
void noteClockSynced() { rtcLastClockSyncEpoch = (uint32_t)time(nullptr); }
static uint32_t &rtcChargerRestartEpoch = rtc.chargerRestartEpoch;
static uint32_t &rtcWifiWindowEpoch = rtc.wifiWindowEpoch;
static bool     &rtcArrivalWindow = rtc.arrivalWindow;

// Must run first in setup(), before anything reads the state above.
static void rtcStateInit() {
    esp_reset_reason_t r = esp_reset_reason();
    if (rtc.magic != RTC_STATE_MAGIC || r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT) {
        memset(&rtc, 0, sizeof(rtc));
        rtc.magic = RTC_STATE_MAGIC;
    }
}

static unsigned long lastFlashPersistMs = 0;
static unsigned long lastPointLogMs = 0;
// Whether GPS was powered on this wake cycle - only true on a fresh boot or
// an ADXL-confirmed motion wake (see setup()). Gates both the GPS ingest
// loop below and the powerOff() call before sleeping, since calling GPS
// UART functions when it was never begin()'d this cycle would talk to an
// unstarted HardwareSerial port. Persists for the whole wake cycle (as
// long as the device stays awake), unlike wifiActive below.
static bool gpsActive = false;
// Whether WiFi/the web server are currently powered on. Starts true under
// the same condition as gpsActive (fresh boot or motion wake) but, unlike
// GPS, is force-disabled again after WIFI_ACTIVE_WINDOW_MS regardless of
// whether motion (and therefore GPS logging) continues - WiFi is a bounded
// opportunity to sync/OTA following movement, not something that needs to
// stay on for the whole active period the way GPS logging does.
static bool wifiActive = false;
static unsigned long wifiOffDeadlineMs = 0; // millis() at which wifiActive should be forced false
// millis() at which the next periodic WiFi recheck is due, while
// continuously active on battery with WiFi currently off (set by
// shutdownWifi(), consumed by the recheck block in loop()). Deliberately
// NOT persisted across sleep/restart (plain static, not RTC_DATA_ATTR):
// it only ever needs to survive within one continuous awake stretch, and
// every path that ends that stretch (deep sleep, or the restart this
// itself triggers) re-derives it from scratch via the next shutdownWifi().
static unsigned long nextWifiRecheckMs = 0;

// Whether the ADXL345 was successfully configured this wake cycle (set in
// setup()). Gates arming the deep-sleep GPIO wake in goToSleep(): the wake
// is level-triggered on LOW, and an unconfigured ADXL drives INT1 push-pull
// idle-LOW (INT_INVERT never set), so arming it anyway makes the chip wake
// the instant it sleeps - an endless reboot loop. See power_manager.h.
static bool motionSensorOk = false;

// The ADXL's own view: lying still (it reported inactivity - 3 s below the
// inactivity threshold - and no activity since). Drives the GPS stillness
// filter in loop(). Starts false (log) until the sensor says otherwise;
// stays false without a working sensor.
static bool adxlStill = false;
// Motion wake not yet confirmed by further movement (see
// UNCONFIRMED_WAKE_TIMEOUT_S) - shorter stationary timeout until it is.
static bool wakeConfirmed = true;
static uint8_t wakeActivityCount = 0;
static void confirmWake(const char *why) {
    if (wakeConfirmed) return;
    wakeConfirmed = true;
    LOGD("[Motion] Wake confirmed (%s) - normal stationary timeout\n", why);
}
// GPS stop/go hysteresis (see GPS_STOP_* in config.h): the receiver's own
// verdict on whether the device moves, from consecutive Doppler speeds.
static bool gpsStopped = false;
static uint8_t gpsSlowFixes = 0;
static uint8_t gpsFastFixes = 0;
static uint8_t gpsFarFixes = 0;   // consecutive good fixes >= the stopped-mode distance
// GPS duty cycle without a fix (see GPS_NOFIX_* in config.h). gpsActive
// stays true while suspended - the GPS is "on for this cycle", just
// powered down between retries.
static bool gpsSuspended = false;
static unsigned long gpsNoFixSinceMs = 0;
static unsigned long gpsResumeMs = 0;
static uint16_t gpsOffCycles = 0;
static uint32_t pointsLoggedThisBoot = 0; // for the idle WiFi recheck interval
// A point was logged at driving speed since the last WiFi window: the next
// stop opens a window at once (see WIFI_MOVED_MIN_SPEED_MPS).
static bool movedSinceWifi = false;
// This boot's WiFi window is the arrival window - no idle close.
static bool arrivalWindowActive = false;
// GPS duty cycle with a fix (see GPS_DUTY_* in config.h).
static bool gpsDutyOff = false;            // module off until gpsDutyOnAtMs
static bool gpsDutyLongOff = false;        // the stopped variant - ADXL activity ends it early
static unsigned long gpsDutyOnAtMs = 0;
static uint8_t gpsGoodFixesSinceOn = 0;
// Module power state with on-time accounting (heartbeat diagnostics).
static bool gpsModuleOn = false;
static unsigned long gpsModuleOnSinceMs = 0; // accounting only - re-based by the heartbeat
static unsigned long gpsOnPhaseStartMs = 0;  // start of the current on-phase
static unsigned long gpsOnAccumMs = 0;
static unsigned long lightSleepAccumMs = 0;

// Every GPS power switch goes through these two. gpsActive ("GPS is part
// of this wake cycle") stays the caller's business; the duty-cycle and
// no-fix states are reset by a power-on from anywhere.
static bool gpsTtffPending = false; // log the time to the first good fix after this power-on
static void gpsPowerOn() {
    gpsReader.begin();
    gpsTtffPending = true;
    gpsModuleOn = true;
    gpsModuleOnSinceMs = gpsOnPhaseStartMs = millis();
    gpsGoodFixesSinceOn = 0;
    gpsDutyOff = false;
    gpsSuspended = false;
}
static void gpsPowerOff() {
    if (!gpsModuleOn) return; // already off - the gate pull-up holds it off
    gpsReader.powerOff();
    gpsReader.end(); // release UART + tri-state pins - see GpsReader::end()
    gpsOnAccumMs += millis() - gpsModuleOnSinceMs;
    gpsModuleOn = false;
}
// millis() since the receiver reports stopped or has no good fix (0 while
// moving) - gates the periodic WiFi recheck to "parked", not "driving".
static unsigned long gpsParkedSinceMs = 0;
// ...and since when without a trustworthy fix at all (indoors) - parked
// sooner than standing still outdoors (see WIFI_STOPPED_OUTDOOR_MS).
static unsigned long gpsUntrustedSinceMs = 0;
static bool hadTrustedFixThisBoot = false;
static float lastTrustedSpeedMps = 0; // speed of the last trustworthy fix
// millis() of the last real ADXL activity interrupt, and since when the
// ADXL reports "moving" without a quiet period (see WAKE_CONFIRM_MOVING_MS).
static unsigned long lastRealActivityMs = 0;
static unsigned long adxlMovingSinceMs = 0;
// After begin() the sensor watches for ACTIVITY first (observed), so a
// device lying still reports nothing at all - no inactivity flag until it
// has moved once. No activity within ADXL_ASSUME_STILL_MS of configuring
// it therefore means still (any real movement fires activity at once).
static bool adxlStateKnown = false;
static unsigned long adxlConfiguredMs = 0;

// Set by the ISR, consumed (and cleared) in loop(). Keep ISR minimal - no
// I2C access from interrupt context.
static volatile bool motionIsrFlag = false;
static void IRAM_ATTR onMotionInterrupt() {
    motionIsrFlag = true;
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
static void shutdownWifi() {
    // Actually powers down the radio (not just stops polling it) - that's
    // where the real current draw is, so this is the step that matters for
    // "WiFi/web server off during sleep/idle" rather than just skipping
    // loop() calls on an object that still exists.
    Log.println("[WiFi] Active window elapsed - powering down radio");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiActive = false;
    // Parked with nothing to upload (backend answered, 0 pending, no point
    // logged this boot): no point rechecking every 5 min - a new point
    // pulls the recheck back in (see the point-logging block in loop()).
    bool idle = syncManager.lastCheckIdle() && pointsLoggedThisBoot == 0;
    nextWifiRecheckMs = millis() + (idle ? WIFI_RECHECK_IDLE_INTERVAL_MS : WIFI_RECHECK_INTERVAL_MS);
    if (idle) LOGD("[WiFi] Nothing to sync and no new points - next recheck in %lu min\n",
                   (unsigned long)(WIFI_RECHECK_IDLE_INTERVAL_MS / 60000UL));
}

// For the boot log line: a crash, watchdog or brownout shows up here on
// the next boot - often the only trace of why the device went offline.
static const char *resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external pin";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "PANIC (crash)";
        case ESP_RST_INT_WDT:   return "INTERRUPT WATCHDOG";
        case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
        case ESP_RST_WDT:       return "WATCHDOG";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        case ESP_RST_PWR_GLITCH: return "POWER GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU LOCKUP";
        default:                return "unknown";
    }
}

bool prepareForRestart() {
    bool saved = trackStore.flushToFlash();
    rtcPlannedRestart = true; // continue the run after restart - see setup()
    rtcGpsWasActive = gpsActive;
    return saved;
}

// For the OPTIONAL restarts (WiFi window refresh, recovery, portal offer,
// charger plug-in): don't restart while RAM holds points the flash couldn't
// take (flash full) - a restart would silently erase them. Returns true if
// the restart may proceed.
// No motion (ADXL activity or GPS-confirmed movement) for at least secs.
static bool stillForSeconds(uint32_t secs) {
    uint32_t epochNow = (uint32_t)time(nullptr);
    return epochNow >= rtcLastMotionEpoch && epochNow - rtcLastMotionEpoch >= secs;
}

// Restarts if possible (does not return then); otherwise backs off for a
// minute so the flush attempt isn't repeated on every loop pass.
// The reason is logged here (only when a restart is actually attempted),
// not by callers - they run every loop pass while in the backoff.
static unsigned long nextOptionalRestartMs = 0;
static unsigned long unsavedSkipSinceMs = 0;
static void tryOptionalRestart(const char *reason) {
    if ((long)(millis() - nextOptionalRestartMs) < 0) return;
    Log.printf("[Power] Restarting: %s\n", reason);
    bool saved = prepareForRestart();
    if (saved) unsavedSkipSinceMs = 0; // a later full-flash episode gets its own hold period
    if (!saved && trackStore.holdsUnsavedPoints()) {
        // With WiFi off this restart is the only way to get WiFi back, and
        // only a sync can free a flash full of unsynced tracks - holding the
        // points would block that for good. Otherwise hold them, but not
        // forever: the points get lost at the next sleep anyway.
        if (unsavedSkipSinceMs == 0) unsavedSkipSinceMs = millis();
        if (wifiActive && millis() - unsavedSkipSinceMs < RESTART_HOLD_FOR_UNSAVED_MS) {
            rtcPlannedRestart = false;
            nextOptionalRestartMs = millis() + 60000UL;
            Log.printf("[Power] ...skipped - %u unsaved points in RAM, flash full\n",
                       (unsigned)trackStore.ramPointCount());
            return;
        }
        Log.printf("[Power] ...dropping %u unsaved points (flash full)\n", (unsigned)trackStore.ramPointCount());
    }
    delay(100);
    ESP.restart();
}

void requestPortalOnNextBoot() {
    rtcOpenPortalOnBoot = true;
}

// Activity that happened while goToSleep() was busy (the pre-sleep flush
// and sync can take many seconds): the device was just picked up, so this
// is not the end of the trip. Reads (and so de-latches) INT_SOURCE.
static bool motionDuringSleepPrep() {
    if (!motionSensorOk) return false;
    bool edge = motionIsrFlag;
    motionIsrFlag = false;
    if (!edge && digitalRead(ADXL_INT_PIN) != LOW) return false;
    return (motionSensor.readAndClearIntSource() & MotionSensor::INT_SRC_ACTIVITY) != 0;
}

// Web UI "Shut down" (see lifecycle.h): close the trip exactly like the
// stationary timeout does, release the peripherals, and sleep with only
// the charger wake armed. Flagged from the HTTP handler and executed at
// the top of loop(), so the response has gone out and nothing runs half
// inside a request handler.
static bool shutdownRequested = false;
void requestShutdown() { shutdownRequested = true; }

// Ends the live trip. One with fewer than MIN_SESSION_POINTS points is
// noise - a night-time bump that got a fix, the device carried around the
// house - and is deleted instead of being kept as a 1-point "track".
static bool closeTrip() {
    String path = trackStore.currentSessionPath();
    uint32_t n = trackStore.sessionPointCount();
    if (path.isEmpty() || n >= MIN_SESSION_POINTS) return trackStore.endSession();
    String date = trackStore.currentSessionDate();
    String file = path.substring(path.lastIndexOf('/') + 1);
    if (LittleFS.exists(path)) {
        trackStore.deleteSession(date, file); // also clears the live-session state
        syncManager.forgetPath(path);
        if (trackStore.listSessionsForDate(date).empty()) trackStore.deleteDate(date);
    } else {
        trackStore.discardCurrentSession(); // never flushed - just drop the RAM points
    }
    Log.printf("[TrackStore] Trip of %lu point(s) discarded as noise: %s\n", (unsigned long)n, path.c_str());
    return true;
}

static void shutdownDevice() {
    Log.println("[Power] Shutting down: closing the trip, releasing GPS/ADXL/WiFi");
    trackStore.flushToFlash();
    closeTrip();
    rtcSessionStartEpoch = 0;
    rtcHasLastPoint = false;
    if (gpsActive) {
        gpsPowerOff();
    }
    if (motionSensorOk) motionSensor.clearInterrupt(); // de-latch INT1 before cutting I2C
    motionSensor.end();
    motionSensor.standby(); // measurement off entirely (~0.1 uA) - nothing wakes on motion anyway
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    powerManager.enterShutdown();
}

// allowMotionAbort=false for the critical-battery sleep: that one must
// happen even while the device is moving.
static void goToSleep(bool allowMotionAbort = true) {
    // Before sleeping: flush RAM to flash so no data is lost, and give any
    // pending sync a brief window if WiFi is already connected.
    trackStore.flushToFlash();
    if (syncManager.isWifiConnected()) {
        syncManager.syncPendingSessions();
    }
    if (allowMotionAbort && motionDuringSleepPrep()) {
        Log.println("[Motion] Activity during pre-sleep flush/sync - staying awake");
        rtcLastMotionEpoch = (uint32_t)time(nullptr);
        // This consumed the activity event the loop's motion handler would
        // otherwise have seen - do its job: a moving device needs GPS.
        if (!gpsActive) {
            gpsPowerOn();
            gpsActive = true;
        }
        return;
    }
    if (powerManager.isCharging()) {
        // Deep sleep is purely a battery-conservation mechanism - pointless
        // while charging, so stay awake. But the stationary timeout IS
        // still the end of the trip: close the session exactly as sleeping
        // would (otherwise an overnight charge appended the next morning's
        // drive to yesterday's file), and power GPS down like sleep does -
        // the motion handler in loop() powers it back up on the next move.
        // Without a working ADXL nothing could power it back up, so GPS
        // then stays on (as before) and just starts a new session.
        Log.println("[Power] Charging - stationary timeout: ending session, staying awake");
        if (closeTrip()) rtcSessionStartEpoch = 0;
        if (gpsActive && motionSensorOk) {
            gpsPowerOff();
            gpsActive = false;
        }
        rtcHasLastPoint = false;
        rtcLastMotionEpoch = (uint32_t)time(nullptr);
        return;
    }
#ifdef DISABLE_DEEP_SLEEP
    // Dev/bench build (see platformio.ini's esp32-c6-devkitm-1-dev env):
    // skip the actual sleep so the USB serial port stays connected
    // continuously instead of dropping/reappearing on every wake cycle.
    Log.println("[Power] DISABLE_DEEP_SLEEP set - would sleep now, staying awake instead");
    rtcLastMotionEpoch = (uint32_t)time(nullptr); // avoid immediately re-triggering every loop
#else
    // Going to sleep because nothing moved for STATIONARY_TIMEOUT_MIN IS
    // the end of the trip: close the session so the next wake's first fix
    // starts a NEW GPX file. Without this, a motion wake resumes (and
    // appends to) the previous trip's file forever - sessions would only
    // ever split at a cold boot or critical battery, silently merging
    // days of separate trips into one track ("I moved today but no new
    // track was created"). A stop longer than the stationary timeout thus
    // deliberately splits the recording into two files.
    closeTrip(); // deep sleep follows - nothing to retry into if it fails
    rtcSessionStartEpoch = 0;
    rtcHasLastPoint = false;

    if (gpsActive) {
        gpsPowerOff();
    }
    if (motionSensorOk) {
        motionSensor.prepareForSleep(); // lower sample rate = lower sleep current
        // De-latch any activity interrupt asserted since loop() last cleared
        // it - the deep-sleep wake is level-triggered on LOW, so a
        // still-latched INT1 would wake the chip the instant it sleeps.
        // Must happen before end() cuts I2C.
        motionSensor.clearInterrupt();
    }
    motionSensor.end(); // release I2C + tri-state SDA/SCL - INT pin untouched
    // Motion wake only when the ADXL was actually configured this cycle -
    // otherwise timer-only sleep (see motionSensorOk above).
    powerManager.enterDeepSleep(SAFETY_WAKE_INTERVAL_SEC, motionSensorOk);
#endif
}

void setup() {
    rtcStateInit();
    setCpuFrequencyMhz(CPU_MHZ_AWAKE); // see config.h - 80 MHz is plenty for everything this runs
#ifdef DISABLE_BROWNOUT_DETECTOR
    // DIAGNOSTIC ONLY - not a fix, and not safe to leave enabled long-term.
    // ESP32-C6 is RISC-V and moved brownout control into a different
    // peripheral than classic Xtensa ESP32/S2/S3 (no RTC_CNTL_BROWN_OUT_REG
    // register/header exists for this chip at all - that's a different,
    // incompatible SoC). esp_brownout_disable() is the portable public API
    // that works across targets instead of poking chip-specific registers
    // directly. This must run before literally anything else, since a
    // brownout reset this early would otherwise wipe out the chip before
    // Serial.begin() even has a chance to run, matching a "totally silent,
    // no boot banner, no ROM output" crash-loop symptom. If disabling this
    // lets the device boot normally, that CONFIRMS a brownout (a voltage
    // sag on power-up, most often from several peripherals drawing current
    // simultaneously at startup) as the cause - but per Espressif's own
    // guidance, disabling BOD on a genuinely underpowered board often just
    // trades a clean reset-loop for undefined/corrupted crashes instead of
    // actually fixing anything. The real fix is addressing the power
    // supply itself: a supply/battery path rated for at least 500mA, a
    // capacitor (e.g. 100-470uF) across 3.3V/GND near the power input to
    // absorb startup current spikes, and short/thick wiring - not leaving
    // the detector permanently disabled, which exists to prevent flash
    // corruption from operating at an unstable voltage.
    esp_brownout_disable();
#endif

    powerManager.captureChargePinAtBoot(); // before anything analog-reads the pin
    Serial.begin(115200);
    // Native USB CDC (no separate USB-UART bridge chip) needs the host to
    // finish enumerating the port before anything written is guaranteed to
    // actually arrive - printing immediately after Serial.begin() can
    // silently drop the first several lines of boot output if the host
    // hasn't caught up yet, which looks identical to a boot-time crash in
    // the monitor (nothing appears) even though the device is running
    // fine. Wait up to 4s for that handshake - but only with USB power
    // present: on battery no host can be there, and waiting anyway cost 4 s
    // of awake time on every wake, including the hourly timer checks.
    if (powerManager.isCharging()) {
        unsigned long serialWaitStart = millis();
        while (!Serial && millis() - serialWaitStart < 4000) {
            delay(10);
        }
    }
    rtcBootCount++;
    Log.printf("\n[Boot #%lu] ESP32-C6 GPS Logger v%s\n", (unsigned long)rtcBootCount, FW_VERSION);

    // Check the battery before anything else - if it's already critical at
    // boot AND not charging, skip WiFi/GPS/sensor bring-up entirely and go
    // straight back to a long sleep. Every wake cycle otherwise brings up
    // WiFi (association attempts) and GPS (power-on) unconditionally before
    // the battery is ever re-checked later in loop() - repeating that
    // hourly on an already-critical cell burns real current for no benefit
    // and can grind a battery down toward a damaging deep-discharge level
    // if left unattended. This is the fastest, lowest-current path back to
    // sleep. If a charger is actively present, though, there's no further
    // drain to protect against, so skip this path and boot normally.
    powerManager.begin();
    bool chargingAtBoot = powerManager.isCharging();
    uint8_t battAtBoot = powerManager.readBatteryPercent();
    // Hysteresis: after a critical sleep, a single unloaded reading must
    // clear a margin before a normal boot - otherwise the cell recovers a
    // few % at rest, boots, sags back under WiFi+GPS load, sleeps again,
    // and repeats all day on exactly the cell this protects.
    bool stayDown = !chargingAtBoot
        && (powerManager.isCritical()
            || (rtcCriticalSleep && battAtBoot < CRITICAL_BATTERY_PERCENT + CRITICAL_RESUME_MARGIN_PERCENT));
    bool wasCriticalSleep = rtcCriticalSleep; // for the wake classification below
    if (!stayDown) rtcCriticalSleep = false;
    if (stayDown) {
        Log.printf("[Power] Critical battery at boot (%u%%) - skipping peripherals, long sleep\n",
                      battAtBoot);
        // The trip (if any) ended when the battery went critical - don't
        // let a later wake resume and append to it.
        rtcSessionStartEpoch = 0;
        rtcHasLastPoint = false;
        rtcCriticalSleep = true;
#ifdef DISABLE_DEEP_SLEEP
        Log.println("[Power] DISABLE_DEEP_SLEEP set - continuing normal boot instead of sleeping");
#else
        motionSensor.standby(); // motion wake is off for this sleep - stop the ADXL's ~140 uA
        // motionWakeEnabled=false: motionSensor.begin() hasn't run yet this
        // cycle, so on a fresh power-up INT1 is at the ADXL's power-on
        // default (active-high polarity, push-pull, idle driven LOW) -
        // arming the level-triggered LOW wake would instant-wake into a
        // fast reboot loop, deep-discharging an already-critical cell.
        // Timer-only wake here; motion wake resumes once the battery
        // recovers enough for a normal boot.
        powerManager.enterDeepSleep(CRITICAL_BATTERY_SLEEP_INTERVAL_SEC, false);
        // does not return
#endif
    }

    // WiFi + the web server (OTA, status, downloads) are gated the same way
    // GPS is: only brought up on a fresh boot or an ADXL-confirmed motion
    // wake, and even then only for a bounded window (WIFI_ACTIVE_WINDOW_MS)
    // rather than the whole time the device stays awake. A safety-timer
    // wake with no motion detected gets neither WiFi nor GPS - it's purely
    // a battery/health check that goes straight back to sleep. This trades
    // away always-on remote reachability (the device is only reachable for
    // WIFI_ACTIVE_WINDOW_MS following actual movement, or via the captive
    // portal case below) for substantially better battery life, since WiFi
    // is one of the biggest power draws on this chip.
    trackStore.begin();   // LittleFS - needed for config + session storage, bounded operation
    // Space reclaim before every track flush - offline too (see SyncManager::reclaimSpace).
    trackStore.setBeforeFlushHook([](size_t need) { syncManager.reclaimSpace(need); });
    configManager.begin();
    Log.begin(configManager.get().debugLog); // records the boot lines above too when on
    Log.printf("[Boot] Reset reason: %s, wake cause: %d\n",
               resetReasonName(esp_reset_reason()), (int)esp_sleep_get_wakeup_cause());

    esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    bool freshBoot = (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED);
    // A PLANNED restart (WiFi-window refresh mid-drive, charger appearing
    // mid-cycle) reports exactly like a fresh boot (UNDEFINED wake cause),
    // but must NOT behave like one: a fresh boot resets the inactivity
    // clock and clears the session, so treating planned restarts as fresh
    // boots turned the periodic WiFi recheck into an infinite restart loop
    // that never sleeps (each restart re-armed the clock, even with the
    // device sitting still) and split every drive into one GPX fragment
    // per restart. The rtcPlannedRestart flag (RTC memory survives a
    // software restart) marks these: continue the run - keep the
    // inactivity clock, resume the session - and just take the fresh WiFi
    // window, which is the entire purpose of the restart.
    bool continuedRun = freshBoot && rtcPlannedRestart;
    rtcPlannedRestart = false;
    if (continuedRun) freshBoot = false;
    bool wokeFromTimer = (wakeCause == ESP_SLEEP_WAKEUP_TIMER);
    // Two deep-sleep wake pins, on two different mechanisms (see
    // PowerManager::enterDeepSleep): ADXL INT1 (motion) uses the GPIO
    // wakeup path triggered LOW, CHARGE_STATUS_PIN (USB power appeared)
    // uses EXT1 triggered HIGH - so the wake cause alone distinguishes them.
    bool wokeFromMotion = (wakeCause == ESP_SLEEP_WAKEUP_GPIO);
    bool wokeFromCharger = (wakeCause == ESP_SLEEP_WAKEUP_EXT1);
    // Plugging in is handling the device: restart the inactivity clock, or
    // the stationary timeout (left over from before the sleep) fires on
    // the very first loop pass.
    if (wokeFromCharger) rtcLastMotionEpoch = (uint32_t)time(nullptr);
    // A planned restart continues a run (wake cause UNDEFINED, so never
    // here); every other motion wake has to prove itself first.
    wakeConfirmed = !wokeFromMotion;

    // GPS only gets powered on and tracked when there's an actual reason to
    // believe the device might have moved: a fresh boot (first power-on -
    // no motion history to go on yet, so assume yes) or an ADXL-confirmed
    // motion wake. A safety-timer-only wake with no motion detected means
    // nothing has moved since the last check, so there's nothing new to
    // fix a location for - skip GPS entirely rather than burning power
    // hunting for a fix on every periodic check-in regardless of movement.
    // Without a working ADXL the last sleep had no motion wake at all, so
    // the hourly timer wake is the only chance to notice a trip - treat it
    // like a motion wake (GPS, WiFi, fresh inactivity clock) instead of
    // never powering GPS/WiFi on battery again after the first sleep.
    // Same when the ADXL configured fine but the motion wake couldn't be
    // armed for that sleep (INT1 stuck LOW, arming error).
    // Not after a critical-battery sleep, whose motion wake was off on
    // purpose: that wake is a plain battery check (a 15-min GPS+WiFi run on
    // a barely recovered cell would sag it straight back to critical).
    bool motionLikeWake = wokeFromMotion
        || (wokeFromTimer && !wasCriticalSleep
            && (rtcMotionSensorFailed || !powerManager.lastSleepHadMotionWake()));
    // A planned restart keeps GPS as it was: turning it back on after the
    // charging stationary timeout had powered it down (e.g. for the setup
    // portal re-offer) started a fresh one-point session every cycle.
    gpsActive = freshBoot || motionLikeWake || (continuedRun && rtcGpsWasActive);
    // WiFi additionally activates whenever charging, independent of motion -
    // deep sleep is already skipped entirely while charging (see
    // goToSleep()), so there's no power-saving reason to keep WiFi off too,
    // and it's specifically needed for NTP sync (SyncManager::
    // ntpSyncIfNeeded(), charging-only) to have any chance to run at all in
    // its intended scenario: charging indoors, stationary, GPS can't get a
    // fix. Without this, a stationary charging device would never bring
    // WiFi up at all, since motion is otherwise the only trigger.
    // ...but not on a motion wake: the device is about to move, and WiFi
    // while moving is useless - the window opens once the movement stops
    // (see the WiFi recheck in loop()). A bump that never turns into a
    // trip then costs no WiFi at all.
    wifiActive = powerManager.isCharging() || (gpsActive && !wokeFromMotion);
    if (wifiActive) {
        rtcWifiWindowEpoch = (uint32_t)time(nullptr);
    } else {
        nextWifiRecheckMs = millis() + WIFI_RECHECK_INTERVAL_MS;
        rtcArrivalWindow = false; // no window this boot - the flag is stale
    }

    // WiFi bring-up. Networks are stored in config.json (up to two - see
    // WifiConnector), so "is WiFi configured" is a plain config check. The
    // setup portal opens when nothing is stored yet, or when the previous
    // run asked for it (no known network reachable while charging and
    // stationary - see loop()). A wake with wifiActive false skips the
    // driver init entirely (keeps timer-only wakes low-power).
    if (wifiActive) {
        WiFi.mode(WIFI_STA);
        configManager.migrateLegacyWifi(); // one-time import of the NVS-stored network
        bool portalRequested = rtcOpenPortalOnBoot;
        rtcOpenPortalOnBoot = false;
        if (portalRequested) {
            configManager.runCaptivePortal(1 - wifiConnector.lastGoodSlot());
        } else if (!configManager.isWifiConfigured()) {
            // Nothing saved: offer setup at power-up/reset, or on the
            // charger once the device is still (re-offered every
            // WIFI_UNCONFIGURED_PORTAL_RETRY_MS then - see loop()). NOT on
            // battery motion wakes or on a car charger while driving: the
            // portal blocks setup() for 3 minutes before GPS starts.
            // (A still-check here would read the motion time from before
            // the last sleep - "still" by definition - so a charger wake at
            // the start of a drive would open it. The loop's re-offer
            // measures stillness live instead.)
            if (freshBoot) {
                configManager.runCaptivePortal(1 - wifiConnector.lastGoodSlot());
            } else {
                Log.println("[WiFi] No network saved - setup portal at power-up, or on the charger once still for 10 min");
            }
        }
        wifiConnector.begin(); // adopts the portal's connection, or starts trying stored networks

        // Saved network(s) but none reachable at a deliberate power-up/reset:
        // give the stored networks a short window, then open the setup portal
        // (AP mode) so a new network can be entered. Only on a fresh boot -
        // wakes and planned restarts must not block GPS start-up for the
        // portal's up-to-3-minute run.
        if (freshBoot && !portalRequested && configManager.isWifiConfigured()) {
            unsigned long t0 = millis();
            while (!wifiConnector.isConnected() && millis() - t0 < WIFI_BOOT_PORTAL_WAIT_MS) {
                wifiConnector.loop();
                delay(100);
            }
            if (!wifiConnector.isConnected()) {
                Log.println("[WiFi] No known network reachable at power-up - setup portal");
                configManager.runCaptivePortal(1 - wifiConnector.lastGoodSlot());
                wifiConnector.begin(); // adopt the portal's connection, or resume retries
            }
        }
    } else {
        Log.println("[WiFi] No motion since last check - skipping WiFi/web server this cycle");
    }

    if (wifiActive) {
        syncManager.begin();
        webUi.begin(); // OTA endpoint is live from this point on, until the active window elapses
        syncManager.requestImmediateSync(); // don't wait for the internal periodic gate within a short window
        // After a drive: WiFi and the web UI stay up until the device
        // sleeps (capped), not just for the usual window.
        arrivalWindowActive = rtcArrivalWindow;
        rtcArrivalWindow = false;
        wifiOffDeadlineMs = millis() + (arrivalWindowActive ? WIFI_ARRIVAL_WINDOW_MS : WIFI_ACTIVE_WINDOW_MS);
        if (arrivalWindowActive) Log.println("[WiFi] Arrival window - stays up until the device sleeps");
    }

    // --- Everything below is peripheral hardware. Best-effort only: none
    // of it can block or fail the WiFi/OTA path above. Small delays are
    // staggered between each .begin() call so their startup current draws
    // don't all spike simultaneously (GPS module power-up, ADXL345's I2C
    // activation, WiFi's own radio current from above, etc. stacking at
    // once is a plausible brownout trigger on a marginal supply/battery) -
    // cheap insurance regardless of whether that's the actual cause here.
    buzzer.begin();
    delay(20);
    motionSensorOk = motionSensor.begin(); // bounded by Wire.setTimeOut(), can't hang boot
    rtcMotionSensorFailed = !motionSensorOk;
    adxlConfiguredMs = millis();
    delay(20);
    if (gpsActive) {
        gpsPowerOn();
    } else {
        Log.println("[GPS] No motion since last check - skipping GPS power-on this cycle");
    }

    // Session identity: on a genuinely fresh boot, clear any previous
    // session so the next real point starts a brand-new trip (lazily, once
    // GPS time is synced - see TrackStore::addPoint). On a wake from sleep
    // that's continuing an existing trip, reconstruct that same session's
    // file path now (deterministic from the stored epoch, no strings need
    // to survive in RTC memory), so points keep appending to the same GPX
    // file instead of fragmenting the trip across multiple files.
    if (freshBoot) {
        rtcSessionStartEpoch = 0;
        rtcHasLastPoint = false;
    } else if (rtcSessionStartEpoch != 0) {
        trackStore.resumeSession(rtcSessionStartEpoch);
    }

    // Any wake that implies real activity (fresh boot or an ADXL activity
    // interrupt) resets the inactivity clock. A safety-timer wake with no
    // motion does NOT reset it, so the device can go straight back to sleep
    // after a quick sync/flush if nothing has moved.
    uint32_t nowEpoch = (uint32_t)time(nullptr);
    if (freshBoot || motionLikeWake) {
        rtcLastMotionEpoch = nowEpoch;
    }

    if (motionSensorOk) {
        attachInterrupt(digitalPinToInterrupt(ADXL_INT_PIN), onMotionInterrupt, FALLING);
    }

    if (!freshBoot) {
        powerManager.printWakeArmDiagnostics();
    }
    if (continuedRun) {
        Log.println("[Boot] Planned restart - continuing run (fresh WiFi window, session/inactivity preserved)");
    } else if (wokeFromMotion) {
        Log.println("[Boot] Woke via ADXL345 motion interrupt");
    } else if (wokeFromCharger) {
        Log.println("[Boot] Woke via USB/charger power appearing");
    } else if (wokeFromTimer) {
        Log.println("[Boot] Woke via safety timer (no motion detected)");
    }

    lastFlashPersistMs = millis();
}

// Re-bases the inactivity clock when the system clock STEPS (first GPS
// fix / NTP): see the comment inside. Called right after the GPS update
// that can cause the step, so every check later in the same loop pass
// (stationary/recent-motion gates, inactivity timeout) already sees the
// re-based value.
static void rebaseInactivityClock() {
    uint32_t nowEpoch = (uint32_t)time(nullptr);
    // Clock-jump compensation: rtcLastMotionEpoch is compared against
    // time(nullptr), but the system clock JUMPS ~56 years forward the moment
    // the first GPS fix (or NTP) syncs it - before that it counts from
    // ~1970. Without re-basing, that jump makes inactiveSeconds instantly
    // exceed STATIONARY_TIMEOUT_MIN and the device sleeps mid-trip, right
    // after acquiring its first fix (in practice: "sleeps N minutes after
    // boot", N = time-to-first-fix). Any forward step far larger than one
    // loop pass (network retries block ~5-15s; threshold 60s) is a sync
    // jump, not elapsed stillness - shift the motion timestamp along with
    // it. A backward step (NTP correcting a fast clock) would leave the
    // timestamp in the future - clamp it back.
    // The step is measured against millis(): a loop pass that genuinely
    // took > 60 s (a long blocking sync pass) is real elapsed time, not a
    // clock step, and must not be "compensated" (which also delayed sleep).
    static uint32_t lastLoopEpoch = 0;
    static unsigned long lastLoopMs = 0;
    unsigned long clockSampleMs = millis(); // same instant as nowEpoch (not the loop-start 'now')
    if (lastLoopEpoch != 0) {
        long clockDelta = (long)(nowEpoch - lastLoopEpoch);
        long realDelta = (long)((clockSampleMs - lastLoopMs) / 1000UL);
        long step = clockDelta - realDelta;
        if (step > 60) {
            rtcLastMotionEpoch += (uint32_t)step;
            Log.printf("[Time] Clock stepped +%lds (time sync) - re-based inactivity clock\n", step);
        }
    }
    if (rtcLastMotionEpoch > nowEpoch) {
        rtcLastMotionEpoch = nowEpoch;
    }
    lastLoopEpoch = nowEpoch;
    lastLoopMs = clockSampleMs;
}

void loop() {
    if (shutdownRequested) shutdownDevice(); // never returns

    // --- GPS ingest (only when powered on this cycle - see setup()) ---
    unsigned long now = millis();
    bool chargingNowRaw = powerManager.isCharging();
    // --- GPS duty cycle with a fix: next on-phase (see config.h) ---
    if (gpsActive && gpsDutyOff && (chargingNowRaw || (long)(now - gpsDutyOnAtMs) >= 0)) {
        gpsPowerOn();
        now = millis(); // begin() waits for the module's supply to settle
    }
    // --- GPS duty cycle without a fix (battery only - see config.h) ---
    if (gpsActive && gpsSuspended) {
        if (chargingNowRaw || (long)(now - gpsResumeMs) >= 0) {
            gpsPowerOn();
            gpsNoFixSinceMs = now;
            LOGD("[GPS] Module back on - %lu min to find a fix\n",
                 (unsigned long)(GPS_NOFIX_RETRY_ON_MS / 60000UL));
        }
    } else if (gpsActive && !gpsDutyOff && !chargingNowRaw) {
        if (gpsReader.hasFix()) {
            gpsNoFixSinceMs = 0;
            if (gpsOffCycles) {
                Log.printf("[GPS] Fix back after %u off period(s)\n", (unsigned)gpsOffCycles);
                gpsOffCycles = 0;
            }
        } else if (gpsNoFixSinceMs == 0) {
            gpsNoFixSinceMs = now;
        }
        unsigned long limit = gpsOffCycles ? GPS_NOFIX_RETRY_ON_MS : GPS_NOFIX_SUSPEND_AFTER_MS;
        if (gpsNoFixSinceMs != 0 && now - gpsNoFixSinceMs >= limit) {
            gpsPowerOff();
            gpsSuspended = true; // after gpsPowerOff() - a later gpsPowerOn() clears it
            gpsResumeMs = now + GPS_NOFIX_OFF_MS;
            if (gpsOffCycles < 65535) gpsOffCycles++;
            Log.printf("[GPS] No fix for %lu min - module off for %lu min (indoors?)\n",
                       (unsigned long)(limit / 60000UL), (unsigned long)(GPS_NOFIX_OFF_MS / 60000UL));
        }
    }

    bool dutyMode = GPS_DUTY_PERIOD_MS > 0 && !chargingNowRaw;
    if (gpsActive && gpsModuleOn) {
        bool newFix = gpsReader.update();
        rebaseInactivityClock(); // an RMC may just have stepped the clock
        // Clock drift diagnostics: the error the first GPS sync of this boot
        // corrected, over the time since the clock was last set exactly
        // (GPS or NTP) - mostly the internal RC slow clock's drift through
        // deep sleep (no 32 kHz crystal on the XIAO). Measured from the last
        // SLEEP ENTRY instead, a night with a fix-less wake in the morning
        // came out as "-224 s in 36 min = -10 %" instead of -0.5 %.
        long stepS;
        if (gpsReader.takeFirstSyncStep(stepS)) {
            uint32_t nowEpoch = (uint32_t)time(nullptr);
            if (rtcLastClockSyncEpoch >= (uint32_t)PLAUSIBLE_EPOCH_FLOOR && nowEpoch > rtcLastClockSyncEpoch + 60) {
                uint32_t elapsed = nowEpoch - rtcLastClockSyncEpoch;
                Log.printf("[Time] First GPS sync: clock was %+ld s off (GPS minus device) %.1f h after the last clock sync = %+.3f %% (device %s)\n",
                           stepS, elapsed / 3600.0, 100.0 * stepS / (double)elapsed,
                           stepS < 0 ? "fast" : "slow");
            } else {
                Log.printf("[Time] First GPS sync: clock was %+ld s off (GPS minus device)\n", stepS);
            }
        }
        // Every GPS clock set (once a second with a fix) moves the reference.
        if (gpsReader.lastClockSetEpoch() > rtcLastClockSyncEpoch) {
            rtcLastClockSyncEpoch = gpsReader.lastClockSetEpoch();
        }

        // hasGoodFix() (not just hasFix()) additionally requires decent
        // HDOP/satellite count, so a track point is only ever logged from
        // a fix precise enough to trust its position - indoor multipath
        // (car -> house: GPS still reports "valid" but chaotically jumps
        // many meters between updates while the device sits still) no
        // longer pollutes the GPX with fake displacement, and no longer
        // gets picked as the "last logged point" that the *next* real
        // point's distance is measured from either.
        // Stillness filter: indoors a lying-still receiver keeps producing
        // "good" fixes (HDOP ~1, 5-14 sats) that jump 5-10 m with a fake
        // 0.4-1.9 m/s Doppler speed - the chaotic scribble at the end of
        // trips. The ADXL knows the device is lying still (see adxlStill);
        // a real trip is logged anyway on speed (driving) or never looks
        // still (walking/carrying - no 3 s of quiet).
        bool stillAndSlow = adxlStill && gpsReader.speedMps() < GPS_STILL_MAX_SPEED_MPS;
        // Stop/go hysteresis on the receiver's Doppler speed - catches
        // what the ADXL filter can't: the device carried around indoors,
        // where the fix stays "good" but drifts with a speed of ~0.
        if (newFix && gpsReader.hasTrustedFix()) { // indoor fixes carry fake speeds
            float v = gpsReader.speedMps();
            if (v < GPS_STOP_MAX_SPEED_MPS) { if (gpsSlowFixes < 255) gpsSlowFixes++; } else gpsSlowFixes = 0;
            if (v >= GPS_GO_MIN_SPEED_MPS) { if (gpsFastFixes < 255) gpsFastFixes++; } else gpsFastFixes = 0;
            static uint32_t stopGoFlips = 0;
            if (!gpsStopped && gpsSlowFixes >= GPS_STOP_FIXES) {
                gpsStopped = true;
                gpsFarFixes = 0;
                stopGoFlips++;
            } else if (gpsStopped && gpsFastFixes >= GPS_GO_FIXES) {
                gpsStopped = false;
                stopGoFlips++;
            }
            // Flips every few seconds while the device is handled indoors -
            // summarise once a minute like the ADXL state.
            static unsigned long lastStopGoLogMs = 0;
            if (stopGoFlips && now - lastStopGoLogMs >= 60000UL) {
                LOGD("[GPS] Receiver reports %s (%lu change(s) in the last minute)\n",
                     gpsStopped ? "stopped" : "moving", (unsigned long)stopGoFlips);
                stopGoFlips = 0;
                lastStopGoLogMs = now;
            }
        }
        bool trusted = gpsReader.hasTrustedFix();
        if (trusted) {
            hadTrustedFixThisBoot = true;
            lastTrustedSpeedMps = gpsReader.speedMps();
        }
        // Standing still outdoors: the receiver says stopped WITH a
        // trustworthy fix. (A missing fix is the indoor case below - while
        // driving it means a tunnel or bad reception, not a stop.)
        bool stoppedOutdoors = gpsStopped && trusted;
        if (!stoppedOutdoors) gpsParkedSinceMs = 0;
        else if (gpsParkedSinceMs == 0) gpsParkedSinceMs = now;
        if (trusted) gpsUntrustedSinceMs = 0;
        else if (gpsUntrustedSinceMs == 0) gpsUntrustedSinceMs = now;
        float minDistM = gpsStopped ? TRACKPOINT_STOPPED_MIN_DISTANCE_M : TRACKPOINT_MIN_DISTANCE_M;
        bool goodNewFix = newFix && gpsReader.hasGoodFix();
        // How long the receiver needed after its power came on - tells
        // whether it hot-starts (seconds) or cold-starts (tens of seconds).
        if (goodNewFix && gpsTtffPending) {
            gpsTtffPending = false;
            Log.printf("[GPS] First fix %.1f s after power-on\n",
                       (now - gpsOnPhaseStartMs) / 1000.0);
        }
        if (goodNewFix && gpsGoodFixesSinceOn < 255) gpsGoodFixesSinceOn++;
        // Duty cycle: the first fix(es) after a power-on are not logged.
        bool settled = !dutyMode || gpsGoodFixesSinceOn >= GPS_DUTY_SETTLE_FIXES;
        if (goodNewFix && gpsReader.hasTrustedFix() && settled && !stillAndSlow) {
            uint8_t battPct = powerManager.readBatteryPercent();

            // Captured before either gets overwritten below - both are
            // needed afterwards to compute positionSpeedMps for the
            // movement vote (see isConfirmedMoving()'s comment).
            bool hadPriorPoint = rtcHasLastPoint;
            // lastPointLogMs is 0 until this boot logs its first point - the
            // reference point itself may be from before a restart/wake.
            bool loggedThisBoot = lastPointLogMs != 0;
            unsigned long msSinceLastPoint = now - lastPointLogMs;

            bool intervalOk = !loggedThisBoot || msSinceLastPoint >= TRACKPOINT_MIN_INTERVAL_MS;
            double dist = hadPriorPoint
                ? gpsReader.distanceFromMeters(rtcLastLoggedLat, rtcLastLoggedLon)
                : minDistM + 1; // always log the first point

            // Seconds since the reference point was logged - from the clock
            // when it was logged before this boot (millis() restarted).
            uint32_t epochNow = (uint32_t)time(nullptr);
            double secsSinceLastPoint = -1; // unknown = no movement vote
            if (loggedThisBoot) {
                secsSinceLastPoint = msSinceLastPoint / 1000.0;
            } else if (rtcLastPointEpoch >= (uint32_t)PLAUSIBLE_EPOCH_FLOOR && epochNow > rtcLastPointEpoch) {
                secsSinceLastPoint = epochNow - rtcLastPointEpoch;
            }

            TrackPoint pt = gpsReader.currentAsTrackPoint(battPct);
            // A point the store drops (clock not synced yet) must not become
            // the reference for the next distance/speed either.
            // Stopped mode: the far position has to persist (see
            // GPS_STOPPED_FAR_FIXES) - a lone drift spike is not a move.
            bool farOk = true;
            if (gpsStopped && hadPriorPoint) {
                if (dist >= minDistM) { if (gpsFarFixes < 255) gpsFarFixes++; } else gpsFarFixes = 0;
                farOk = gpsFarFixes >= GPS_STOPPED_FAR_FIXES;
            }
            if (intervalOk && (dist >= minDistM || !hadPriorPoint) && farOk
                    && trackStore.addPoint(pt)) {
                gpsFarFixes = 0;
                pointsLoggedThisBoot++;
                if (!wifiActive && gpsReader.speedMps() >= WIFI_MOVED_MIN_SPEED_MPS) movedSinceWifi = true;
                // A stretched (idle) recheck is no longer appropriate once
                // the device logs points again.
                if (!wifiActive && (long)(nextWifiRecheckMs - now) > (long)WIFI_RECHECK_INTERVAL_MS) {
                    nextWifiRecheckMs = now + WIFI_RECHECK_INTERVAL_MS;
                }
                lastPointLogMs = now;
                rtcLastPointEpoch = pt.epoch;
                rtcLastLoggedLat = gpsReader.lat();
                rtcLastLoggedLon = gpsReader.lon();
                rtcHasLastPoint = true;
                // addPoint() lazily starts a session on the very first point of
                // a trip (see TrackStore) - persist its epoch so a subsequent
                // deep-sleep wake can resume the same session/file instead of
                // starting a new one (see setup()'s resumeSession() call).
                rtcSessionStartEpoch = trackStore.currentSessionStartEpoch();
                // GPS-confirmed movement counts as activity for the sleep
                // decision, independent of the accelerometer: smooth driving
                // (highway cruising) can stay under the ADXL threshold for
                // longer than STATIONARY_TIMEOUT_MIN, and the device would
                // otherwise fall asleep MID-DRIVE. Gated on
                // isConfirmedMoving(): hasGoodFix() alone isn't a strong
                // enough filter against indoor multipath on this receiver
                // (see its comment in gps_reader.h) - positionSpeedMps
                // (distance-since-last-point / elapsed time) is what's
                // actually relied on to tell real movement from jitter.
                // Only meaningful when hadPriorPoint is true (dist is a
                // real measured distance, not the first-point-of-session
                // sentinel above); otherwise pass 0 so it never spuriously
                // counts as "moving" from that sentinel value. A fix that
                // fails the speed check simply withholds its vote here - it
                // does NOT force sleep by itself; the ADXL path below still
                // independently resets rtcLastMotionEpoch on real physical
                // motion regardless of what the GPS is doing.
                double positionSpeedMps = (hadPriorPoint && secsSinceLastPoint > 0)
                    ? dist / secsSinceLastPoint
                    : 0.0;
                if (gpsReader.isConfirmedMoving(positionSpeedMps)) {
                    rtcLastMotionEpoch = (uint32_t)time(nullptr);
                    confirmWake("GPS movement");
                }
            }
        }

        // Duty cycle: the settled fix has been evaluated - switch the module
        // off until the next one is due (see GPS_DUTY_* in config.h).
        if (dutyMode && goodNewFix && settled) {
            bool stoppedNow = gpsStopped || stillAndSlow;
            unsigned long onMs = now - gpsOnPhaseStartMs;
            unsigned long offMs = stoppedNow ? GPS_DUTY_STOPPED_OFF_MS
                : (onMs + GPS_DUTY_MIN_OFF_MS >= GPS_DUTY_PERIOD_MS ? GPS_DUTY_MIN_OFF_MS
                                                                    : GPS_DUTY_PERIOD_MS - onMs);
            gpsPowerOff();
            gpsDutyOff = true; // after gpsPowerOff() - a later gpsPowerOn() clears it
            gpsDutyLongOff = stoppedNow;
            gpsDutyOnAtMs = now + offMs;
        }
    }

    // --- Motion interrupt handling (deferred I2C access out of ISR) ---
    bool motionIsrSeen = motionIsrFlag;
    bool motionDetected = motionIsrSeen;
    motionIsrFlag = false;
    // Latch-recovery fallback: the ADXL activity interrupt LATCHES INT1 LOW
    // until INT_SOURCE is read, and the ESP only notices it via a FALLING
    // edge. If activity fired in the window between motionSensor.begin() and
    // attachInterrupt() in setup() (seconds apart - WiFi/GPS bring-up sits in
    // between, and the board is often literally in-hand then), the pin is
    // already LOW when the ISR is finally attached, no edge ever arrives,
    // and motion detection stays dead for the whole wake cycle. Polling the
    // level catches that latched-but-unseen case.
    if (!motionDetected && motionSensorOk && digitalRead(ADXL_INT_PIN) == LOW) {
        motionDetected = true;
    }
    // Also poll INT_SOURCE regularly: inactivity has no wired interrupt
    // (INT2), but its latched flag is what tells "lying still" apart from
    // "moving" for the stillness filter (see adxlStill).
    static unsigned long lastIntPollMs = 0;
    bool statePollDue = motionSensorOk && now - lastIntPollMs >= ADXL_STATE_POLL_MS;
    if (motionDetected || statePollDue) {
        lastIntPollMs = now;
        // Verify against the ADXL itself before counting this as motion: a
        // falling edge on the INT wire alone proves nothing - electrical
        // glitches (WiFi TX current bursts coupling into the wiring/supply)
        // fire the GPIO ISR just the same. Only the ACTIVITY bit actually
        // set in INT_SOURCE resets the inactivity clock; anything else is
        // logged as spurious so it's visible but can't hold the device
        // awake forever. (The read also de-latches INT1 either way.)
        uint8_t intSource = motionSensor.readAndClearIntSource();
        bool realActivity = (intSource & MotionSensor::INT_SRC_ACTIVITY) != 0;
        // LINK mode alternates: activity -> (3 s quiet) inactivity ->
        // activity... Both flags in one read means activity followed an
        // inactivity (or a long loop stall hid the order) - count it as
        // moving, the safe side for logging.
        bool wasStill = adxlStill;
        if (realActivity) {
            adxlStill = false;
            adxlStateKnown = true;
        } else if (intSource & MotionSensor::INT_SRC_INACTIVITY) {
            adxlStill = true;
            adxlStateKnown = true;
        } else if (!adxlStateKnown && now - adxlConfiguredMs >= ADXL_ASSUME_STILL_MS) {
            adxlStill = true;
            adxlStateKnown = true;
        }
        // Continuous movement confirms a wake too (see WAKE_CONFIRM_MOVING_MS).
        if (adxlStateKnown && !adxlStill) {
            if (adxlMovingSinceMs == 0) adxlMovingSinceMs = now;
            else if (!wakeConfirmed && now - adxlMovingSinceMs >= WAKE_CONFIRM_MOVING_MS) {
                confirmWake("continuous movement");
            }
        } else {
            adxlMovingSinceMs = 0;
        }
        // Transitions flip several times a minute while driving - log the
        // state at most once a minute (with the flip count), not each flip.
        static unsigned long lastStillLogMs = 0;
        static uint32_t stillFlips = 0;
        if (adxlStill != wasStill) stillFlips++;
        if (stillFlips && now - lastStillLogMs >= 60000UL) {
            LOGD("[Motion] ADXL: %s (%lu change(s) in the last minute)\n",
                 adxlStill ? "still" : "moving", (unsigned long)stillFlips);
            stillFlips = 0;
            lastStillLogMs = now;
        }
        if (realActivity) {
            rtcLastMotionEpoch = (uint32_t)time(nullptr);
            lastRealActivityMs = now;
            if (!wakeConfirmed && ++wakeActivityCount >= WAKE_CONFIRM_ACTIVITY_COUNT) {
                confirmWake("repeated accelerometer activity");
            }
            // Picked up / driving off during a stopped duty-cycle pause:
            // don't wait out the 30 s.
            if (gpsDutyOff && gpsDutyLongOff) gpsDutyOnAtMs = now;
            // GPS re-activation for motion during a no-GPS wake cycle:
            // gpsActive is decided at boot from the WAKE REASON, but motion
            // can start mid-cycle - device picked up right after an hourly
            // timer wake, or driven off while charging (charger wake). The
            // motion keeps resetting the inactivity clock, so without this
            // the device would stay awake for the whole trip with GPS off:
            // nothing logged, and no sleep->motion-wake to recover either.
            if (!gpsActive) {
                Log.println("[GPS] Motion during a no-GPS wake cycle - powering GPS on");
                gpsPowerOn();
                gpsActive = true;
            }
        }
        // Log (rate-limited - continuous movement latches this many times a
        // second) so motion detection is directly testable on serial: plug
        // in USB (device stays awake while charging) and shake the board.
        // Suffix: how it was noticed (edge = ISR, poll = latch-recovery
        // above) and INT1's level right after de-latching ("re-asserted"
        // repeating while the board sits STILL means a stuck/noisy line).
        // Real activity fires every few seconds while driving or carried
        // (400+ lines an hour - the 48 KB log held less than an hour of a
        // trip): summarise it once a minute. Spurious edges and a
        // re-asserted INT1 stay individually logged (5 s throttle) - those
        // are what a wiring/noise diagnosis needs to see.
        static unsigned long lastMotionLogMs = 0;
        static unsigned long lastActivityLogMs = 0;
        static uint32_t activityCount = 0;
        bool reasserted = digitalRead(ADXL_INT_PIN) == LOW;
        if (realActivity && !reasserted) {
            activityCount++;
            if (now - lastActivityLogMs >= 60000UL) {
                LOGD("[Motion] Activity: %lu interrupt(s) in the last minute (last via %s)\n",
                     (unsigned long)activityCount, motionIsrSeen ? "edge" : "poll");
                activityCount = 0;
                lastActivityLogMs = now;
            }
        } else if ((realActivity || motionDetected) && now - lastMotionLogMs >= 5000) { // not routine state polls
            Log.printf("[Motion] %s (%s, INT_SOURCE=0x%02X, INT1 %s after clear)\n",
                          realActivity ? "Activity" : "Spurious INT edge - not motion",
                          motionIsrSeen ? "edge" : "poll", intSource,
                          reasserted ? "re-asserted" : "idle-high");
            lastMotionLogMs = now;
        }
    }

    // --- Motion noise diagnostic (tuning aid, off by default): track the
    //     peak sample-to-sample acceleration delta and report it every 5s.
    //     This measures what the ADXL actually sees, in mg, so
    //     ADXL_ACT_THRESHOLD_MG can be set from data instead of guesses -
    //     enable with -D MOTION_DEBUG_RAW=1 in platformio.ini when tuning
    //     (e.g. after adding supply decoupling at the sensor to see whether
    //     the noise floor dropped enough for a lower threshold). Peaks
    //     matching [Motion] Activity events = real signal reaching the
    //     sensor; Activity events WITHOUT peaks = the activity engine or
    //     the INT wiring. ---
#ifdef MOTION_DEBUG_RAW
    if (motionSensorOk) {
        static float lastMgX = 0, lastMgY = 0, lastMgZ = 0;
        static bool havePrevSample = false;
        static float peakDeltaMg = 0;
        static unsigned long lastPeakLogMs = 0;
        float mgX, mgY, mgZ;
        if (motionSensor.readAccel(mgX, mgY, mgZ)) {
            if (havePrevSample) {
                float d = max(fabsf(mgX - lastMgX), max(fabsf(mgY - lastMgY), fabsf(mgZ - lastMgZ)));
                if (d > peakDeltaMg) peakDeltaMg = d;
            }
            lastMgX = mgX; lastMgY = mgY; lastMgZ = mgZ;
            havePrevSample = true;
        }
        if (now - lastPeakLogMs >= 5000) {
            if (peakDeltaMg >= 40.0f) {
                Log.printf("[Motion] peak sample delta last 5s: %.0f mg\n", peakDeltaMg);
            }
            peakDeltaMg = 0;
            lastPeakLogMs = now;
        }
    }
#endif // MOTION_DEBUG_RAW

    // --- Periodic flash persistence (every 15 min) ---
    if (now - lastFlashPersistMs >= FLASH_PERSIST_INTERVAL_MS) {
        trackStore.flushToFlash();
        lastFlashPersistMs = now;
    }

    // --- Battery checks ---
    bool critical = powerManager.isCritical();
    bool low = powerManager.isLow();

    // Critical only matters on battery - the same rule setup() applies at
    // boot. While charging, goToSleep() returns straight away, so this
    // branch used to run on EVERY loop pass: each pass closed the session
    // (every GPS point became its own GPX file - ~1200 files/hour driving
    // on a car charger with a flat cell) and ran a sync pass.
    static bool wasLow = false;
    if (critical && !powerManager.isCharging()) {
        buzzer.update(false); // silence before sleep; critical state has its own urgency, no point draining further
        trackStore.endSession();
        rtcSessionStartEpoch = 0; // session is finalized/closed - next wake should start a new trip, not resume this one
        rtcCriticalSleep = true;  // next boot must clear the resume margin (see setup())
        goToSleep(false);         // no motion abort - this sleep must happen
    } else if (low && !wasLow) {
        // One forced flush on ENTERING low battery, independent of the
        // 15-min timer, then back to the normal cadence. (A time-based
        // re-arm fired again every second, flushing each point on its own
        // for the whole low-battery period.)
        trackStore.flushToFlash();
        lastFlashPersistMs = now;
        wasLow = true;
    }
    // Re-armed only clearly above the threshold: the reading jitters by ~1%
    // around 15%, which re-entered "low" (and re-flushed) every few seconds.
    if (wasLow && powerManager.readBatteryPercent() > LOW_BATTERY_PERCENT + 2) wasLow = false;
    // Storage almost full (only not-uploaded tracks left to fill it - they
    // are never deleted): beep in daytime so the user uploads them before
    // recording stops. Re-evaluated every 10 s (storageStatus() is cached).
    static bool storageAlert = false;
    static unsigned long lastStorageCheckMs = 0;
    if (lastStorageCheckMs == 0 || now - lastStorageCheckMs >= 10000UL) {
        lastStorageCheckMs = now;
        bool daytime = false;
        time_t t = time(nullptr);
        if (t >= PLAUSIBLE_EPOCH_FLOOR) { // unknown time of day = no beeping
            struct tm lt;
            localtime_r(&t, &lt);
            daytime = lt.tm_hour >= STORAGE_BEEP_DAY_START_HOUR && lt.tm_hour < STORAGE_BEEP_DAY_END_HOUR;
        }
        storageAlert = daytime && syncManager.storageStatus().level >= 1;
#ifdef STORAGE_BEEP_TEST
        storageAlert = true; // bench test of the pattern: build with -D STORAGE_BEEP_TEST=1
#endif
    }
    buzzer.update(low && !critical && !powerManager.isCharging(), storageAlert);

    // --- Charger state, debounced for the transition handlers below ---
    // Intermittent 5V (misaligned Qi pad, loose car USB) would otherwise
    // fire the restart-on-plug-in and the unplug grace window on every
    // flicker.
    static bool chargingStable = powerManager.isCharging();
    static bool chargingRawPrev = chargingStable;
    static unsigned long chargingRawSinceMs = now;
    bool chargingRaw = powerManager.isCharging();
    if (chargingRaw != chargingRawPrev) {
        chargingRawPrev = chargingRaw;
        chargingRawSinceMs = now;
    }
    static unsigned long chargingStableSinceMs = now;
    if (chargingRaw != chargingStable && now - chargingRawSinceMs >= CHARGE_DEBOUNCE_MS) {
        chargingStable = chargingRaw;
        chargingStableSinceMs = now;
    }

    // --- Charger appears while the radio is shut down mid-cycle ---
    // wifiActive is decided once at boot and, on battery, force-disabled
    // after WIFI_ACTIVE_WINDOW_MS. Charging can then START mid-cycle (USB
    // plugged into an awake device) - without handling, the device stays
    // awake indefinitely with the web UI unreachable. Resurrecting the
    // Arduino WiFi driver from WIFI_OFF in place proved UNRELIABLE in
    // practice (core 3.x deinitializes the driver on WIFI_OFF; the mid-
    // flight re-init "starts" but never associates - verified by testing),
    // so take the provably-good path instead: external power is available
    // now, flush everything and restart into the normal fresh-boot WiFi
    // bring-up. Costs ~3 seconds and splits any in-progress session at the
    // plug-in point. No restart loop is possible: after the reboot,
    // charging makes wifiActive true from setup(), so this branch (which
    // requires !wifiActive) can't re-trigger.
    // Only for a charger that stays (CHARGER_RESTART_STABLE_MS), and at most
    // once per CHARGER_RESTART_MIN_INTERVAL_S: 2026-10-06 a false charging
    // reading restarted the device 27 times in 30 minutes of driving.
    uint32_t chgEpoch = (uint32_t)time(nullptr);
    bool chgRestartAllowed = rtcChargerRestartEpoch == 0 || chgEpoch < rtcChargerRestartEpoch
        || chgEpoch - rtcChargerRestartEpoch >= CHARGER_RESTART_MIN_INTERVAL_S;
    if (!wifiActive && chargingStable && now - chargingStableSinceMs >= CHARGER_RESTART_STABLE_MS
            && chgRestartAllowed) {
        uint32_t prevEpoch = rtcChargerRestartEpoch;
        rtcChargerRestartEpoch = chgEpoch;
        tryOptionalRestart("charger power appeared mid-cycle - normal boot flow for WiFi");
        rtcChargerRestartEpoch = prevEpoch; // skipped - not used up
    }

    // --- Charging-stop grace window ---
    // While charging, the WiFi active-window cutoff below is suspended, but
    // its deadline timestamp keeps aging - after a long charge millis() is
    // far past it, so without this block unplugging the cable kills WiFi
    // INSTANTLY ("device went offline the moment USB came out"). Grant one
    // fresh window on the charging->battery transition: the UI stays
    // reachable for a final look/sync, then the radio powers down normally
    // and the inactivity path leads to sleep.
    static int8_t prevCharging = -1; // -1 = not sampled yet (first pass)
    bool chargingNow = chargingStable;
    if (prevCharging == 1 && !chargingNow) {
        // Unplugging is a physical interaction with the device - reset the
        // inactivity clock too. While charging, that clock only gets reset
        // when the timeout bounces off goToSleep()'s charging check, so at
        // the moment of unplug it can already be AT the timeout - without
        // this, the device could fall asleep the very instant the cable
        // leaves, before the WiFi grace window below means anything.
        rtcLastMotionEpoch = (uint32_t)time(nullptr);
        if (wifiActive) {
            Log.println("[WiFi] Charger disconnected - granting one fresh WiFi active window");
            wifiOffDeadlineMs = millis() + WIFI_ACTIVE_WINDOW_MS;
        }
    }
    prevCharging = chargingNow ? 1 : 0;

    // --- WiFi / sync / web UI (bounded active window - see setup()) ---
    if (wifiActive) {
#ifndef DISABLE_DEEP_SLEEP
        // Dev builds keep WiFi on indefinitely for a stable debug
        // connection (see the -dev platformio.ini env) - production
        // builds enforce the cutoff, except while charging: deep sleep is
        // already skipped entirely then (see goToSleep()), so there's no
        // power-saving reason to force WiFi off either, and NTP sync
        // specifically needs it to stay up while charging (see setup()).
        // Debounced charger state, like the plug-in/unplug handlers above:
        // the raw reading dropped WiFi on the first pass after an unplug
        // (or a 1-s dropout), before the unplug grace window could grant a
        // fresh window - and the still-"charging" debounced state then
        // restarted the device as "charger appeared".
        if (millis() >= wifiOffDeadlineMs && !chargingStable) {
            shutdownWifi(); // sets wifiActive = false
        }
        // On battery, a window in which neither saved network is in range
        // is pointless after the first scans (no network = no sync, no
        // web UI either): close it early instead of scanning for 5 min.
        // A drive with no known WiFi otherwise spent half its time with
        // the radio scanning - a large share of the battery drain.
        static unsigned long noneInRangeSinceMs = 0;
        if (wifiActive && !chargingStable && wifiConnector.noneInRange()) {
            if (noneInRangeSinceMs == 0) noneInRangeSinceMs = millis();
            if (millis() - noneInRangeSinceMs >= WIFI_NONE_IN_RANGE_CLOSE_MS) {
                Log.println("[WiFi] Neither saved network in range - closing the window early");
                shutdownWifi();
            }
        } else {
            noneInRangeSinceMs = 0;
        }
        // On battery, a connected window that found nothing to upload and
        // serves no web requests only keeps the radio on for nobody: close
        // it early. A page left open polls /api/status and keeps it alive.
        if (wifiActive && !arrivalWindowActive && !chargingStable && syncManager.isWifiConnected() && syncManager.lastCheckIdle()
                && millis() >= WIFI_IDLE_CLOSE_MS
                && millis() - webUi.lastRequestMs() >= WIFI_IDLE_CLOSE_MS) {
            Log.println("[WiFi] Nothing to sync and no web UI activity - closing the window early");
            shutdownWifi();
        }
#endif
    }

    // --- Periodic WiFi recheck during a continuous-motion stretch ---
    // wifiActive is decided once at boot and, on its own, never comes back
    // after WIFI_ACTIVE_WINDOW_MS elapses - fine for a device that goes
    // back to sleep between trips (each sleep/wake gets a fresh window),
    // but a long stretch of continuous motion (hours on the road) means
    // rtcLastMotionEpoch keeps getting reset before STATIONARY_TIMEOUT_MIN,
    // so the device never sleeps either - it just runs on with WiFi off
    // for the rest of the drive, only ever getting the ONE window from the
    // wake that started it. In practice: it only actually syncs once
    // charging starts back home, exactly as reported. Resurrecting the
    // WiFi driver from WIFI_OFF in place is unreliable on this core (see
    // the charger-mid-cycle handler above, which hit the same problem) -
    // so, the same way that handler does, get a fresh window the only way
    // proven to work here: flush and restart into a normal boot. The
    // restart reports as a fresh boot (ESP_SLEEP_WAKEUP_UNDEFINED), which
    // re-evaluates wifiActive true and brings WiFi up again for another
    // WIFI_ACTIVE_WINDOW_MS - a real chance to pass within range of a
    // known network mid-drive and flush pending sessions, not just at the
    // very start and very end of a long trip. RtcState (track session,
    // inactivity clock, etc.) survives the restart same as it does across
    // deep sleep, so nothing is lost beyond the ~3s restart itself. Gated
    // to !wifiActive (this window's the one that's already closed), actual
    // motion within the last recheck interval (gpsActive alone also held
    // after a single bump on a desk, costing a restart and a second WiFi
    // window for nothing), and battery (charging keeps WiFi on anyway).
    // Also gated to "parked": the receiver reporting stopped (or no fix -
    // indoors) for WIFI_RECHECK_STILL_MS. At driving speed no association
    // completes anyway, and every recheck cost a restart plus a minute of
    // scanning - the largest avoidable drain on a commute.
    uint32_t recheckEpoch = (uint32_t)time(nullptr);
    bool recentMotion = recheckEpoch >= rtcLastMotionEpoch
        && recheckEpoch - rtcLastMotionEpoch < WIFI_RECHECK_INTERVAL_MS / 1000UL;
    // Indoors (no trustworthy fix) 45 s is enough; standing still outdoors
    // with a good fix needs 2 minutes - a red light is not an arrival.
    // "No trustworthy fix" alone is not parked: the accelerometer must be
    // quiet as well (no activity, no continuous movement), and a fix lost at
    // driving speed means a tunnel or bad reception, not an arrival.
    uint32_t quietS = recheckEpoch >= rtcLastMotionEpoch ? recheckEpoch - rtcLastMotionEpoch : 0;
    unsigned long untrustedMs = gpsUntrustedSinceMs != 0 ? millis() - gpsUntrustedSinceMs : 0;
    bool parkedIndoors = untrustedMs >= WIFI_RECHECK_STILL_MS
        && quietS * 1000UL >= WIFI_RECHECK_STILL_MS
        && (lastTrustedSpeedMps < WIFI_MOVED_MIN_SPEED_MPS || untrustedMs >= WIFI_LOST_FIX_PARKED_MS);
    bool parkedLongEnough = parkedIndoors
        || (gpsParkedSinceMs != 0 && millis() - gpsParkedSinceMs >= WIFI_STOPPED_OUTDOOR_MS);
    // The regular window waits for the GPS to have spoken (or 10 min).
    bool intervalRecheckDue = (long)(millis() - nextWifiRecheckMs) >= 0
        && (hadTrustedFixThisBoot || millis() >= WIFI_NOFIX_FIRST_RECHECK_MS);
    // After a drive, the first stop opens a window right away (arrival) -
    // spaced WIFI_STOP_WINDOW_MIN_GAP_S apart; otherwise the interval
    // applies. Never for a bump that wasn't confirmed as a trip.
    bool stopAfterDrive = movedSinceWifi
        && (rtcWifiWindowEpoch == 0 || recheckEpoch < rtcWifiWindowEpoch
            || recheckEpoch - rtcWifiWindowEpoch >= WIFI_STOP_WINDOW_MIN_GAP_S);
    if (!wifiActive && gpsActive && wakeConfirmed && recentMotion && !chargingStable && parkedLongEnough
            && configManager.isWifiConfigured() // nothing to connect to otherwise
            && (stopAfterDrive || intervalRecheckDue)) {
        rtcArrivalWindow = stopAfterDrive;
        tryOptionalRestart(stopAfterDrive ? "stopped after a drive - WiFi window"
                                          : "periodic WiFi recheck while parked");
        rtcArrivalWindow = false; // skipped - not used up
    }

    if (wifiActive) {
        // Connect/disconnect transitions only - every intermediate status
        // code of each retry (6, 1, ...) flooded the debug log during a long
        // offline stretch. WifiConnector logs the attempts themselves.
        static bool wasConnected = false;
        bool connectedNow = WiFi.status() == WL_CONNECTED;
        if (connectedNow != wasConnected) {
            LOGD("[WiFi] %s\n", connectedNow ? "Link up" : "Link down");
            wasConnected = connectedNow;
            // Possibly the other site's network now - forget the backend
            // resolved on the previous one and lift any give-up.
            if (connectedNow) syncManager.onNetworkConnected();
        }

        // Network retry/alternation, then offline-streak recovery (see
        // WIFI_STUCK_RESTART_MS in config.h): while charging and still
        // without any known network after the in-place attempts, offer the
        // setup portal once if stationary (how a network gets added at a
        // new location), else restart once to recover a wedged driver.
        wifiConnector.loop();
        if (wifiConnector.isConnected()) {
            rtcWifiPortalOffered = false;
            rtcWifiRecoveryRestartDone = false;
        } else if (chargingStable && configManager.isWifiConfigured()
                   && wifiConnector.unconnectedForMs() >= WIFI_STUCK_RESTART_MS) {
            // No motion (ADXL or GPS) for WIFI_PORTAL_STATIONARY_S - long
            // enough that a stop at a red light or in a jam on a car
            // charger doesn't qualify (the portal blocks GPS for 3 min).
            bool stationary = stillForSeconds(WIFI_PORTAL_STATIONARY_S);
            // Each step is marked used only by a restart that actually
            // happens: tryOptionalRestart() returns only when it skipped (or
            // is backing off), and then the step is given back - a skip used
            // to burn both steps within two loop passes, neither having run.
            // Both steps used up: keep alternating in place, no restart loop.
            if (stationary && !rtcWifiPortalOffered) {
                rtcWifiPortalOffered = true;
                rtcOpenPortalOnBoot = true;
                tryOptionalRestart("no known network reachable - into the setup portal");
                rtcWifiPortalOffered = false;
                rtcOpenPortalOnBoot = false;
            } else if (!rtcWifiRecoveryRestartDone) {
                rtcWifiRecoveryRestartDone = true;
                tryOptionalRestart("still unconnected after in-place retries - normal boot flow");
                rtcWifiRecoveryRestartDone = false;
            }
        } else if (chargingStable && !configManager.isWifiConfigured()
                   && now >= WIFI_UNCONFIGURED_PORTAL_RETRY_MS
                   && stillForSeconds(WIFI_PORTAL_STATIONARY_S)) {
            // Nothing saved and on the charger: the portal from setup()
            // timed out (e.g. the backend was still being set up) - offer
            // it again instead of never (the other restart paths need a
            // saved network, battery power, or WiFi off). Only while still:
            // a purely offline logger on a car charger must not lose 3
            // minutes of GPS to the portal every 10 minutes of driving.
            rtcOpenPortalOnBoot = true;
            tryOptionalRestart("no network saved - reopening the setup portal");
            rtcOpenPortalOnBoot = false;
        }

        webUi.loop();
        if (syncManager.isWifiConnected()) {
            syncManager.loop();
        }
    }

    // --- Inactivity check -> deep sleep if no ADXL motion interrupt for
    //     STATIONARY_TIMEOUT_MIN. No periodic GPS/coordinate polling needed;
    //     the accelerometer interrupt is the sole source of "did it move".
    //     WiFi's own bounded active window (above) is independent of this -
    //     WiFi turning off early doesn't by itself trigger sleep, and this
    //     firing doesn't wait on WiFi either, since a timer-only wake never
    //     had WiFi active in the first place. ---
    rebaseInactivityClock();
    uint32_t nowEpoch = (uint32_t)time(nullptr);
    // The live session can end outside the logging path (web UI delete,
    // charging timeout) - keep the resumable copy in step, so a planned
    // restart can't resurrect a deleted/closed session.
    if (trackStore.currentSessionPath().isEmpty()) rtcSessionStartEpoch = 0;
    // Continuous movement (the ADXL reports "moving" - no 3-s quiet since its
    // last activity interrupt) holds the stationary timeout: few new
    // interrupts arrive then, and before the GPS has its first fix nothing
    // else would keep a bike ride or a walk awake. Capped, see
    // ADXL_MOVING_HOLD_MAX_MS.
    if (motionSensorOk && adxlStateKnown && !adxlStill && lastRealActivityMs != 0
            && now - lastRealActivityMs < ADXL_MOVING_HOLD_MAX_MS) {
        rtcLastMotionEpoch = nowEpoch;
    }
    uint32_t inactiveSeconds = (nowEpoch >= rtcLastMotionEpoch) ? (nowEpoch - rtcLastMotionEpoch) : 0;
    if (inactiveSeconds >= (uint32_t)STATIONARY_TIMEOUT_MIN * 60UL) {
        Log.println("[Motion] No ADXL activity for timeout period - sleeping");
        goToSleep(); // does not return, UNLESS built with DISABLE_DEEP_SLEEP (dev/bench env) or charging
    }
    // Unconfirmed motion wake (a bump): back to sleep sooner - only once the
    // ADXL says still again, and not on a charger (nothing to save there).
    if (!wakeConfirmed && motionSensorOk && adxlStateKnown && adxlStill && !chargingStable
            && inactiveSeconds >= UNCONFIRMED_WAKE_TIMEOUT_S) {
        Log.printf("[Motion] Woken by a bump - no further movement for %u s, back to sleep\n",
                   (unsigned)UNCONFIRMED_WAKE_TIMEOUT_S);
        goToSleep();
    }

    // --- Heartbeat (see HEARTBEAT_INTERVAL_MS in config.h) ---
    static unsigned long lastHeartbeatMs = 0;
    if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
        Log.printf("[Heartbeat] up=%lus wifi=%s ip=%s chg=%d batt=%u%% gps=%d inact=%lus\n",
                      now / 1000UL,
                      wifiActive ? (WiFi.status() == WL_CONNECTED ? "connected" : "on-unconnected") : "off",
                      wifiActive && WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
                      (int)powerManager.isCharging(),
                      powerManager.readBatteryPercent(),
                      (int)gpsActive,
                      (unsigned long)inactiveSeconds);
        // Share of the last interval with the GPS module powered and with
        // the CPU in light sleep - what the duty cycle actually achieves.
        unsigned long intervalMs = lastHeartbeatMs ? now - lastHeartbeatMs : now;
        unsigned long gpsOnMs = gpsOnAccumMs + (gpsModuleOn ? now - gpsModuleOnSinceMs : 0);
        if (gpsModuleOn) gpsModuleOnSinceMs = now;
        gpsOnAccumMs = 0;
        unsigned gpsOnPct = intervalMs ? (unsigned)min<unsigned long>(100, gpsOnMs * 100UL / intervalMs) : 0;
        unsigned sleepPct = intervalMs ? (unsigned)min<unsigned long>(100, lightSleepAccumMs * 100UL / intervalMs) : 0;
        lightSleepAccumMs = 0;
        LOGD("[Heartbeat] heap=%u (min %u) rssi=%d flash=%u/%uKB pending=%d sats=%u fix=%d ram_pts=%u gps_on=%u%% lsleep=%u%%\n",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
             WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
             (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024),
             wifiActive ? syncManager.countPendingSessionsCached() : -1,
             (unsigned)gpsReader.satellites(), (int)gpsReader.hasFix(),
             (unsigned)trackStore.ramPointCount(), gpsOnPct, sleepPct);
        lastHeartbeatMs = now;
    }

    Log.loop(); // saves the debug log to flash periodically (no-op when off)

#if LIGHT_SLEEP_ENABLED
    // --- Light sleep while the GPS module is off (see config.h) ---
    // Battery, radio off, no beep running, nothing pending: sleep until the
    // next GPS on-phase, at most one slice, so the accelerometer flag,
    // battery and timers keep being looked at once a second. millis()
    // keeps counting through light sleep.
    if (gpsActive && !gpsModuleOn && !wifiActive && !chargingNowRaw && !buzzer.busy()
            && !shutdownRequested) {
        unsigned long t = millis();
        unsigned long untilOn = LIGHT_SLEEP_SLICE_MS;
        if (gpsDutyOff) untilOn = (long)(gpsDutyOnAtMs - t) > 0 ? gpsDutyOnAtMs - t : 0;
        else if (gpsSuspended) untilOn = (long)(gpsResumeMs - t) > 0 ? gpsResumeMs - t : 0;
        unsigned long ms = min(untilOn, (unsigned long)LIGHT_SLEEP_SLICE_MS);
        if (ms >= 20) {
            esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
            esp_light_sleep_start();
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
            lightSleepAccumMs += millis() - t;
            return;
        }
    }
#endif
    delay(10); // yield
}
