#include "power_manager.h"
#include "motion_sensor.h"
#include <esp_sleep.h>
#include <Arduino.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"

PowerManager powerManager;

// Diagnostics for the deep-sleep wake sources, persisted across the sleep
// in RTC memory: the pre-sleep serial log is invisible in the field (sleep
// only ever happens unplugged), so the arm results are stored here and
// printed at the NEXT boot instead (printWakeArmDiagnostics()).
RTC_DATA_ATTR static int32_t rtcChargerArmErr = INT32_MIN;  // esp_err_t, INT32_MIN = no sleep yet
RTC_DATA_ATTR static int32_t rtcMotionArmErr = INT32_MIN;   // esp_err_t, -1 = deliberately not armed
RTC_DATA_ATTR static int32_t rtcChargePinAtSleep = INT32_MIN;

void PowerManager::begin() {
    analogReadResolution(12);
    pinMode(BATTERY_ADC_PIN, ANALOG); 
    pinMode(CHARGE_STATUS_PIN, INPUT); 
    readBatteryPercent(); 
}

void PowerManager::printWakeArmDiagnostics() {
    if (rtcChargerArmErr == INT32_MIN) return; // no sleep recorded yet
    Serial.printf("[Power] Previous sleep arm results: charger wake err=%ld (pin was %ld at sleep entry), "
                  "motion wake err=%ld (0=OK, -1=not armed), charge pin now=%d\n",
                  (long)rtcChargerArmErr, (long)rtcChargePinAtSleep,
                  (long)rtcMotionArmErr, digitalRead(CHARGE_STATUS_PIN));
}

bool PowerManager::isCharging() {
    // Read the 5V-presence divider as ANALOG, not digital: the 100k/100k
    // divider puts ~2.5V on this pin, which sits almost exactly on the
    // C6's digital VIH threshold (0.75 * 3.3V = 2.475V). digitalRead()
    // therefore flickers with VBUS sag/temperature - and every piece of
    // charging-dependent logic (stay awake, WiFi re-activation on replug,
    // unplug grace window, NTP) flickers with it. Against an analog 1.5V
    // threshold the same signal is unambiguous: ~2.3-2.6V plugged, ~0V
    // unplugged. NOTE: the deep-sleep EXT1 plug-in wake still depends on
    // the LP pad's DIGITAL threshold - only the 180k bottom-resistor swap
    // (see config.h) fixes that side; this fixes everything while awake.
    return analogReadMilliVolts(CHARGE_STATUS_PIN) > 1500;
}

float PowerManager::readBatteryVoltage() {
    uint32_t sum = 0;
    const int N = 8;
    
    for (int i = 0; i < N; i++) {
        sum += analogReadMilliVolts(BATTERY_ADC_PIN); 
        delayMicroseconds(200);
    }
    float averageMv = sum / (float)N;
    float trueBatteryMv = averageMv * BATTERY_DIVIDER_RATIO; 
    
    return trueBatteryMv / 1000.0f;
}

uint8_t PowerManager::readBatteryPercent() {
    // Rate-limit the actual measurement: loop() calls this at least twice
    // per pass (isCritical + isLow) at up to ~100 Hz, which would advance
    // the EMA below ~200x/s - converging on any momentary reading in well
    // under a second and nullifying the smoothing entirely (a brief WiFi
    // TX voltage sag could then trip a false "critical battery" sleep).
    // Sampling at most every 2s keeps the EMA a real low-pass filter and
    // drops ~1600 wasted ADC reads per second on the side.
    unsigned long nowMs = millis();
    if (_lastSampleMs != 0 && (nowMs - _lastSampleMs) < 2000) {
        return (uint8_t)lroundf(_lastPct);
    }

    float mv = readBatteryVoltage() * 1000.0f;
    
    if (mv < 2000.0f) return 0; 

    const float volts[] = { 3200.0f, 3500.0f, 3680.0f, 3780.0f, 3930.0f, 4050.0f, 4150.0f };
    const float pcts[]  = {    0.0f,    5.0f,   15.0f,   40.0f,   70.0f,   90.0f,  100.0f };
    const int numPoints = 7;

    float pct = 0.0f;

    if (mv >= volts[numPoints - 1]) {
        pct = 100.0f;
    } else if (mv <= volts[0]) { // Behoben: Array-Index [0] hinzugefügt
        pct = 0.0f;
    } else {
        for (int i = 0; i < numPoints - 1; i++) {
            if (mv >= volts[i] && mv < volts[i+1]) {
                float fraction = (mv - volts[i]) / (volts[i+1] - volts[i]);
                pct = pcts[i] + fraction * (pcts[i+1] - pcts[i]);
                break;
            }
        }
    }

    pct = constrain(pct, 0.0f, 100.0f);

    if (_lastSampleMs == 0) {
        _lastPct = pct; // first-ever reading seeds the filter
    } else {
        // Float EMA - see the member comment: an integer-stored EMA
        // truncates away its own convergence and stalls short of the target.
        _lastPct = 0.7f * _lastPct + 0.3f * pct;
    }
    _lastSampleMs = nowMs;

    return (uint8_t)lroundf(_lastPct);
}

bool PowerManager::isLow() {
    return readBatteryPercent() <= LOW_BATTERY_PERCENT;
}

bool PowerManager::isCritical() {
    return readBatteryPercent() <= CRITICAL_BATTERY_PERCENT;
}

void PowerManager::enterDeepSleep(uint32_t sleepSeconds, bool motionWakeEnabled) {
    Serial.printf("[Power] Entering deep sleep (motion wake %s, charger wake armed, safety timer %lus)\n",
                  motionWakeEnabled ? "armed" : "SKIPPED - ADXL not configured this cycle",
                  (unsigned long)sleepSeconds);
    Serial.flush();

    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);

    // USB/charger plug-in wake: CHARGE_STATUS_PIN (GPIO0, an LP_IO pin) goes
    // HIGH via the 5V-pin divider the moment USB power appears. Without this,
    // a sleeping device plugged into USB stays asleep (dead serial port, no
    // web UI) until the next motion or timer wake - it only LOOKS crashed.
    // Sleep is only ever entered while NOT charging (see goToSleep() and the
    // critical-battery path), so this pin is reliably LOW right now and a
    // HIGH level-trigger can't fire instantly the way the motion pin could.
    //
    // Armed via EXT1 (LP-pad wakeup), NOT esp_deep_sleep_enable_gpio_wakeup:
    // the HIGH-level mode of the plain GPIO-wakeup path is a known trouble
    // spot on the C6 and never fired here in practice, while EXT1 is the
    // commonly-reported-working route for this chip. The internal LP pulls
    // must be OFF: the divider has ~100k source impedance, so a ~45k
    // internal pulldown would drag the plugged-in ~2.5V down to ~1.2V -
    // never HIGH - even though awake digitalRead()s look fine. The divider
    // itself defines the level in both states (bottom leg to GND when
    // unplugged), no internal pull needed.
    // Reclaim the pin as a DIGITAL input before arming the wake: the awake
    // code samples this pin through the ADC (isCharging()), which leaves
    // the pad in analog mode with its digital input buffer disabled - and
    // goToSleep() calls isCharging() right before getting here. Armed in
    // that state, the EXT1 level detector never sees the pin go high, and
    // the plug-in wake silently stops working (verified: the wake worked
    // before isCharging() switched to analog reads and failed after).
    pinMode(CHARGE_STATUS_PIN, INPUT);
    rtc_gpio_init((gpio_num_t)CHARGE_STATUS_PIN);
    rtc_gpio_set_direction((gpio_num_t)CHARGE_STATUS_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis((gpio_num_t)CHARGE_STATUS_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)CHARGE_STATUS_PIN);
    rtcChargePinAtSleep = digitalRead(CHARGE_STATUS_PIN);
    rtcChargerArmErr = (int32_t)esp_sleep_enable_ext1_wakeup(1ULL << CHARGE_STATUS_PIN,
                                                             ESP_EXT1_WAKEUP_ANY_HIGH);

    if (motionWakeEnabled) {
        // Last-resort guard: the wake is LEVEL-triggered on LOW, so arming
        // it while INT1 is already asserted guarantees an instant wake and
        // a reboot loop. The caller de-latches the interrupt before this
        // (see goToSleep()), but if the pin still reads LOW - stale latch,
        // wiring fault, sensor in an unexpected state - sleep on the safety
        // timer alone this cycle instead.
        if (digitalRead(ADXL_INT_PIN) == LOW) {
            Serial.println("[Power] ADXL INT1 already asserted (LOW) - motion wake skipped this cycle");
            Serial.flush();
            rtcMotionArmErr = -1;
        } else {
            uint64_t wakeMask = MotionSensor::wakeupPinMask();
            rtcMotionArmErr = (int32_t)esp_deep_sleep_enable_gpio_wakeup(wakeMask, ESP_GPIO_WAKEUP_GPIO_LOW);
        }
    } else {
        rtcMotionArmErr = -1;
    }

    esp_deep_sleep_start();
    while (true) { }
}
