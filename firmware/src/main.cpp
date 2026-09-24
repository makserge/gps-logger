#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include "config.h"
#include "config_manager.h"
#include "track_store.h"
#include "gps_reader.h"
#include "power_manager.h"
#include "motion_sensor.h"
#include "sync_manager.h"
#include "web_server_ui.h"
#include "buzzer.h"
#ifdef DISABLE_BROWNOUT_DETECTOR
#include "esp_private/brownout.h"
#endif

// -----------------------------------------------------------------------
// RTC (deep-sleep-persistent) state
// -----------------------------------------------------------------------
RTC_DATA_ATTR static double  rtcLastLoggedLat = 0;
RTC_DATA_ATTR static double  rtcLastLoggedLon = 0;
RTC_DATA_ATTR static bool    rtcHasLastPoint = false;
RTC_DATA_ATTR static uint32_t rtcLastMotionEpoch = 0;   // updated on every ADXL activity interrupt
RTC_DATA_ATTR static uint32_t rtcSessionStartEpoch = 0;
RTC_DATA_ATTR static uint32_t rtcBootCount = 0;

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

// Whether the ADXL345 was successfully configured this wake cycle (set in
// setup()). Gates arming the deep-sleep GPIO wake in goToSleep(): the wake
// is level-triggered on LOW, and an unconfigured ADXL drives INT1 push-pull
// idle-LOW (INT_INVERT never set), so arming it anyway makes the chip wake
// the instant it sleeps - an endless reboot loop. See power_manager.h.
static bool motionSensorOk = false;

// Set by the ISR, consumed (and cleared) in loop(). Keep ISR minimal - no
// I2C access from interrupt context.
static volatile bool motionIsrFlag = false;
static void IRAM_ATTR onMotionInterrupt() {
    motionIsrFlag = true;
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
static void startWifiNonBlocking() {
    // Initialize the driver BEFORE checking for credentials: WiFi.SSID()
    // (behind isWifiConfigured()) reads empty while the radio is off, so
    // checking first silently starts nothing - exactly the state after
    // shutdownWifi() set WIFI_OFF and a charger replug tries to bring the
    // radio back mid-cycle. (Same landmine as the boot flow fixed earlier.)
    WiFi.mode(WIFI_STA);
    if (configManager.isWifiConfigured()) {
        WiFi.begin();
    } else {
        WiFi.mode(WIFI_OFF); // nothing saved - don't leave an idle radio on
    }
}

static void shutdownWifi() {
    // Actually powers down the radio (not just stops polling it) - that's
    // where the real current draw is, so this is the step that matters for
    // "WiFi/web server off during sleep/idle" rather than just skipping
    // loop() calls on an object that still exists.
    Serial.println("[WiFi] Active window elapsed - powering down radio");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiActive = false;
}

static void goToSleep() {
    // Before sleeping: flush RAM to flash so no data is lost, and give any
    // pending sync a brief window if WiFi is already connected.
    trackStore.flushToFlash();
    if (syncManager.isWifiConnected()) {
        syncManager.syncPendingSessions();
    }
    if (powerManager.isCharging()) {
        // Deep sleep is purely a battery-conservation mechanism - pointless
        // (and counterproductive, since it'd also cut GPS/logging) while
        // actively charging. Stay awake instead; reset the inactivity
        // clock so this doesn't get re-evaluated (and re-logged) on every
        // single loop pass while charging.
       // Serial.println("[Power] Charging - deep sleep disabled, staying awake");
        rtcLastMotionEpoch = (uint32_t)time(nullptr);
        return;
    }
#ifdef DISABLE_DEEP_SLEEP
    // Dev/bench build (see platformio.ini's esp32-c6-devkitm-1-dev env):
    // skip the actual sleep so the USB serial port stays connected
    // continuously instead of dropping/reappearing on every wake cycle.
    Serial.println("[Power] DISABLE_DEEP_SLEEP set - would sleep now, staying awake instead");
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
    trackStore.endSession();
    rtcSessionStartEpoch = 0;

    if (gpsActive) {
        gpsReader.powerOff();
        gpsReader.end(); // release UART + tri-state pins - see GpsReader::end()
    }
    if (motionSensorOk) {
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

    Serial.begin(115200);
    // Native USB CDC (no separate USB-UART bridge chip) needs the host to
    // finish enumerating the port before anything written is guaranteed to
    // actually arrive - printing immediately after Serial.begin() can
    // silently drop the first several lines of boot output if the host
    // hasn't caught up yet, which looks identical to a boot-time crash in
    // the monitor (nothing appears) even though the device is running
    // fine. Wait up to 4s for that handshake, but bounded - deployed/
    // field operation (running on battery, no USB/monitor attached at all)
    // must never hang here waiting for something that isn't coming.
    unsigned long serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 4000) {
        delay(10);
    }
    rtcBootCount++;
    Serial.printf("\n[Boot #%lu] ESP32-C6 GPS Logger v%s\n", (unsigned long)rtcBootCount, FW_VERSION);

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
    if (powerManager.isCritical() && !powerManager.isCharging()) {
        Serial.printf("[Power] Critical battery at boot (%u%%) - skipping peripherals, long sleep\n",
                      powerManager.readBatteryPercent());
#ifdef DISABLE_DEEP_SLEEP
        Serial.println("[Power] DISABLE_DEEP_SLEEP set - continuing normal boot instead of sleeping");
#else
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
    configManager.begin();

    esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    bool freshBoot = (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED);
    bool wokeFromTimer = (wakeCause == ESP_SLEEP_WAKEUP_TIMER);
    // Two deep-sleep wake pins, on two different mechanisms (see
    // PowerManager::enterDeepSleep): ADXL INT1 (motion) uses the GPIO
    // wakeup path triggered LOW, CHARGE_STATUS_PIN (USB power appeared)
    // uses EXT1 triggered HIGH - so the wake cause alone distinguishes them.
    bool wokeFromMotion = (wakeCause == ESP_SLEEP_WAKEUP_GPIO);
    bool wokeFromCharger = (wakeCause == ESP_SLEEP_WAKEUP_EXT1);

    // GPS only gets powered on and tracked when there's an actual reason to
    // believe the device might have moved: a fresh boot (first power-on -
    // no motion history to go on yet, so assume yes) or an ADXL-confirmed
    // motion wake. A safety-timer-only wake with no motion detected means
    // nothing has moved since the last check, so there's nothing new to
    // fix a location for - skip GPS entirely rather than burning power
    // hunting for a fix on every periodic check-in regardless of movement.
    gpsActive = freshBoot || wokeFromMotion;
    // WiFi additionally activates whenever charging, independent of motion -
    // deep sleep is already skipped entirely while charging (see
    // goToSleep()), so there's no power-saving reason to keep WiFi off too,
    // and it's specifically needed for NTP sync (SyncManager::
    // ntpSyncIfNeeded(), charging-only) to have any chance to run at all in
    // its intended scenario: charging indoors, stationary, GPS can't get a
    // fix. Without this, a stationary charging device would never bring
    // WiFi up at all, since motion is otherwise the only trigger.
    wifiActive = gpsActive || powerManager.isCharging();

    // WiFi bring-up. IMPORTANT: isWifiConfigured() reads the credentials
    // via WiFi.SSID(), which only works once the WiFi driver is initialized
    // (WiFi.mode(WIFI_STA)) - checked before that, it ALWAYS returns empty,
    // which used to send every single boot through the blocking WiFiManager
    // autoConnect() path. That mostly worked by luck, but any failed
    // association then opened the 3-minute config portal AP and left the
    // device offline for the whole wake cycle with no retry. Initialize STA
    // first, then decide. A wake with wifiActive false skips the driver
    // init entirely (keeps timer-only wakes low-power); the captive portal
    // still runs on a genuinely unconfigured device, since a fresh boot
    // always has wifiActive true.
    if (wifiActive) {
        WiFi.mode(WIFI_STA); // init driver so stored credentials are readable
        if (!configManager.isWifiConfigured()) {
            configManager.runCaptivePortal();
        } else {
            startWifiNonBlocking(); // non-blocking connect with saved credentials
        }
    } else {
        Serial.println("[WiFi] No motion since last check - skipping WiFi/web server this cycle");
    }

    if (wifiActive) {
        syncManager.begin();
        webUi.begin(); // OTA endpoint is live from this point on, until the active window elapses
        syncManager.requestImmediateSync(); // don't wait for the internal periodic gate within a short window
        wifiOffDeadlineMs = millis() + WIFI_ACTIVE_WINDOW_MS;
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
    delay(20);
    if (gpsActive) {
        gpsReader.begin();
    } else {
        Serial.println("[GPS] No motion since last check - skipping GPS power-on this cycle");
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
    if (freshBoot || wokeFromMotion) {
        rtcLastMotionEpoch = nowEpoch;
    }

    if (motionSensorOk) {
        attachInterrupt(digitalPinToInterrupt(ADXL_INT_PIN), onMotionInterrupt, FALLING);
    }

    if (!freshBoot) {
        powerManager.printWakeArmDiagnostics();
    }
    if (wokeFromMotion) {
        Serial.println("[Boot] Woke via ADXL345 motion interrupt");
    } else if (wokeFromCharger) {
        Serial.println("[Boot] Woke via USB/charger power appearing");
    } else if (wokeFromTimer) {
        Serial.println("[Boot] Woke via safety timer (no motion detected)");
    }

    lastFlashPersistMs = millis();
}

void loop() {
    // --- GPS ingest (only when powered on this cycle - see setup()) ---
    unsigned long now = millis();
    if (gpsActive) {
        bool newFix = gpsReader.update();

        if (newFix && gpsReader.hasFix()) {
            uint8_t battPct = powerManager.readBatteryPercent();

            bool intervalOk = (now - lastPointLogMs) >= TRACKPOINT_MIN_INTERVAL_MS;
            double dist = rtcHasLastPoint
                ? gpsReader.distanceFromMeters(rtcLastLoggedLat, rtcLastLoggedLon)
                : TRACKPOINT_MIN_DISTANCE_M + 1; // always log the first point

            if (intervalOk && (dist >= TRACKPOINT_MIN_DISTANCE_M || !rtcHasLastPoint)) {
                TrackPoint pt = gpsReader.currentAsTrackPoint(battPct);
                trackStore.addPoint(pt);
                lastPointLogMs = now;
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
                // otherwise fall asleep MID-DRIVE. A logged point already
                // means >= TRACKPOINT_MIN_DISTANCE_M of real displacement
                // (drift-debounced), so it's a trustworthy "we are moving".
                rtcLastMotionEpoch = (uint32_t)time(nullptr);
            }
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
    if (motionDetected) {
        // Verify against the ADXL itself before counting this as motion: a
        // falling edge on the INT wire alone proves nothing - electrical
        // glitches (WiFi TX current bursts coupling into the wiring/supply)
        // fire the GPIO ISR just the same. Only the ACTIVITY bit actually
        // set in INT_SOURCE resets the inactivity clock; anything else is
        // logged as spurious so it's visible but can't hold the device
        // awake forever. (The read also de-latches INT1 either way.)
        uint8_t intSource = motionSensor.readAndClearIntSource();
        bool realActivity = (intSource & MotionSensor::INT_SRC_ACTIVITY) != 0;
        if (realActivity) {
            rtcLastMotionEpoch = (uint32_t)time(nullptr);
            // GPS re-activation for motion during a no-GPS wake cycle:
            // gpsActive is decided at boot from the WAKE REASON, but motion
            // can start mid-cycle - device picked up right after an hourly
            // timer wake, or driven off while charging (charger wake). The
            // motion keeps resetting the inactivity clock, so without this
            // the device would stay awake for the whole trip with GPS off:
            // nothing logged, and no sleep->motion-wake to recover either.
            if (!gpsActive) {
                Serial.println("[GPS] Motion during a no-GPS wake cycle - powering GPS on");
                gpsReader.begin();
                gpsActive = true;
            }
        }
        // Log (rate-limited - continuous movement latches this many times a
        // second) so motion detection is directly testable on serial: plug
        // in USB (device stays awake while charging) and shake the board.
        // Suffix: how it was noticed (edge = ISR, poll = latch-recovery
        // above) and INT1's level right after de-latching ("re-asserted"
        // repeating while the board sits STILL means a stuck/noisy line).
        static unsigned long lastMotionLogMs = 0;
        if (now - lastMotionLogMs >= 5000) {
            Serial.printf("[Motion] %s (%s, INT_SOURCE=0x%02X, INT1 %s after clear)\n",
                          realActivity ? "Activity" : "Spurious INT edge - not motion",
                          motionIsrSeen ? "edge" : "poll", intSource,
                          digitalRead(ADXL_INT_PIN) == LOW ? "re-asserted" : "idle-high");
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
                Serial.printf("[Motion] peak sample delta last 5s: %.0f mg\n", peakDeltaMg);
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

    if (critical) {
       // Serial.println("[Power] Critical battery - flushing and sleeping");
        buzzer.update(false); // silence before sleep; critical state has its own urgency, no point draining further
        trackStore.endSession();
        rtcSessionStartEpoch = 0; // session is finalized/closed - next wake should start a new trip, not resume this one
        goToSleep();
    } else if (low && (now - lastFlashPersistMs) > 1000) {
        // Force an immediate flush on low battery, independent of the 15-min
        // timer - and re-arm the persist clock, otherwise this branch would
        // fire again on every subsequent loop pass, turning each new GPS
        // point into its own individual flash write for as long as the
        // battery stays low (heavy LittleFS wear exactly when power is
        // scarce). One forced flush now, then back to the normal cadence.
        trackStore.flushToFlash();
        lastFlashPersistMs = now;
    }
    buzzer.update(low && !critical && !powerManager.isCharging());

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
    if (!wifiActive && powerManager.isCharging()) {
        Serial.println("[WiFi] Charger power appeared mid-cycle - restarting into normal boot flow");
        trackStore.flushToFlash();
        delay(100);
        ESP.restart();
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
    bool chargingNow = powerManager.isCharging();
    if (prevCharging == 1 && !chargingNow) {
        // Unplugging is a physical interaction with the device - reset the
        // inactivity clock too. While charging, that clock only gets reset
        // when the timeout bounces off goToSleep()'s charging check, so at
        // the moment of unplug it can already be AT the timeout - without
        // this, the device could fall asleep the very instant the cable
        // leaves, before the WiFi grace window below means anything.
        rtcLastMotionEpoch = (uint32_t)time(nullptr);
        if (wifiActive) {
            Serial.println("[WiFi] Charger disconnected - granting one fresh WiFi active window");
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
        if (millis() >= wifiOffDeadlineMs && !powerManager.isCharging()) {
            shutdownWifi(); // sets wifiActive = false
        }
#endif
    }
    if (wifiActive) {
        // Log status transitions (not every loop iteration) so connection
        // failures show up on serial for diagnosis without flooding it.
        static wl_status_t lastLoggedWifiStatus = (wl_status_t)255;
        wl_status_t wifiStatus = WiFi.status();
        if (wifiStatus != lastLoggedWifiStatus) {
            Serial.printf("[WiFi] status: %d%s\n", (int)wifiStatus,
                          wifiStatus == WL_CONNECTED ? " (connected)" : "");
            lastLoggedWifiStatus = wifiStatus;
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
    static uint32_t lastLoopEpoch = 0;
    if (lastLoopEpoch != 0 && nowEpoch > lastLoopEpoch + 60) {
        uint32_t jump = nowEpoch - lastLoopEpoch;
        rtcLastMotionEpoch += jump;
        Serial.printf("[Time] Clock jumped +%lus (first time sync) - re-based inactivity clock\n",
                      (unsigned long)jump);
    }
    if (rtcLastMotionEpoch > nowEpoch) {
        rtcLastMotionEpoch = nowEpoch;
    }
    lastLoopEpoch = nowEpoch;
    uint32_t inactiveSeconds = (nowEpoch >= rtcLastMotionEpoch) ? (nowEpoch - rtcLastMotionEpoch) : 0;
    if (inactiveSeconds >= (uint32_t)STATIONARY_TIMEOUT_MIN * 60UL) {
        Serial.println("[Motion] No ADXL activity for timeout period - sleeping");
        goToSleep(); // does not return, UNLESS built with DISABLE_DEEP_SLEEP (dev/bench env) or charging
    }

    delay(10); // yield
}
