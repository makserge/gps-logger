#pragma once
#include "config.h"

class PowerManager {
public:
    void begin();
    uint8_t readBatteryPercent();  // 0-100, smoothed
    float readBatteryVoltage();
    bool isLow();       // <= LOW_BATTERY_PERCENT
    bool isCritical();  // <= CRITICAL_BATTERY_PERCENT

    // True if 5V/USB-C power appears to be present (i.e. the XIAO's onboard
    // charger is actively charging), inferred via a resistor divider off
    // the board's 5V pin into CHARGE_STATUS_PIN (the XIAO ESP32-C6 has no
    // dedicated CHRG/STAT line broken out). Digital HIGH = charging.
    bool isCharging();

    // Deep sleep with RTC wake sources configured:
    //  - GPIO wakeup on ADXL_INT_PIN (motion) - ESP32-C6 deep-sleep GPIO
    //    wakeup only supports its LP_IO pins (GPIO0-GPIO7). Only armed when
    //    motionWakeEnabled is true - see the call site in main.cpp. Arming
    //    this unconditionally is dangerous: ESP_GPIO_WAKEUP_GPIO_LOW is
    //    level-triggered, so if the ADXL never got configured this cycle
    //    (begin() failed) INT1 sits at its power-on default of idle-LOW,
    //    or if a real activity interrupt was never de-latched before
    //    Wire.end() cut I2C, the pin is already asserted - either way the
    //    chip wakes the instant it sleeps and reboot-loops forever.
    //  - GPIO wakeup on CHARGE_STATUS_PIN going HIGH (USB/charger power
    //    appeared) - always armed: sleep is only ever entered while not
    //    charging, so the pin is reliably LOW at this point and can't
    //    trigger instantly the way the motion pin could.
    //  - a timer safety-net wake, defaulting to SAFETY_WAKE_INTERVAL_SEC but
    //    overridable (e.g. CRITICAL_BATTERY_SLEEP_INTERVAL_SEC when already
    //    critical at boot - see main.cpp) so the device still periodically
    //    wakes even with zero motion, at whatever cadence makes sense
    [[noreturn]] void enterDeepSleep(uint32_t sleepSeconds = SAFETY_WAKE_INTERVAL_SEC,
                                      bool motionWakeEnabled = true);

    // Prints the RTC-persisted results of the previous sleep's wake-source
    // arming (esp_deep_sleep_enable_gpio_wakeup return codes + the charge
    // pin's level at sleep entry). The pre-sleep serial log is invisible in
    // the field (sleep only happens unplugged), so call this at boot after
    // a wake to diagnose why a wake source didn't fire. No-op before the
    // first sleep.
    void printWakeArmDiagnostics();

private:
    // EMA state kept as FLOAT: stored as an integer, the smoothing update
    // (0.7*old + 0.3*new) truncates its fractional progress every cycle and
    // permanently stalls a few percent short of the target (e.g. stuck at
    // "97%" on a full battery, since 0.7*97+0.3*100 = 97.9 -> 97 forever).
    float _lastPct = 100.0f;
    // millis() of the last real ADC sample; 0 = never sampled yet. Gates
    // readBatteryPercent()'s measurement to at most one every 2s so the
    // EMA smoothing isn't nullified by high-frequency callers (see .cpp).
    unsigned long _lastSampleMs = 0;
};

extern PowerManager powerManager;
