#pragma once
#include "config.h"

// Minimal register-level ADXL345 driver. We only use the ACTIVITY interrupt
// (routed to INT1) as a hardware motion trigger:
//  - While awake: INT1 fires an ESP32 GPIO interrupt on every burst of
//    motion, which the main loop uses to reset an inactivity timer.
//  - While asleep: INT1 is configured as a deep-sleep GPIO wakeup source,
//    so the device only wakes when it's actually moved, instead of on a
//    fixed timer to re-check GPS coordinates.
// The interrupt polarity is inverted (INT_INVERT) so INT1 idles HIGH and
// pulses LOW on activity, which is what ESP_GPIO_WAKEUP_GPIO_LOW expects.
class MotionSensor {
public:
    bool begin();

    // INT_SOURCE (0x30) bit for a genuine activity event. Other bits (e.g.
    // DATA_READY) are always churning and don't mean motion.
    static constexpr uint8_t INT_SRC_ACTIVITY = 0x10;

    // Reads INT_SOURCE (which also clears/de-latches INT1/INT2) and returns
    // it, so the caller can check INT_SRC_ACTIVITY - a falling edge on the
    // INT wire is NOT proof of motion by itself (electrical glitches, e.g.
    // WiFi TX bursts coupling into the wiring, fire the GPIO ISR too).
    uint8_t readAndClearIntSource();

    // Same read, result discarded - for callers that only want to de-latch.
    void clearInterrupt();

    // Read the current acceleration sample in mg (10-bit, +-2g range).
    // Returns false if the I2C read failed. Used by the noise diagnostic in
    // loop() to measure how much apparent motion the sensor actually sees,
    // so ADXL_ACT_THRESHOLD_MG can be tuned from data instead of guesses.
    bool readAccel(float &xMg, float &yMg, float &zMg);

    // Combined GPIO wake bitmask helper (this pin's bit), used by PowerManager.
    static uint64_t wakeupPinMask();

    // Releases the I2C bus and tri-states SDA/SCL (INPUT, no pull) before
    // sleep - same rationale as GpsReader::end(): stops the ESP from
    // driving/pulling the bus lines while other things on that bus are
    // meant to be powered down, and drops any leakage through Wire's
    // internal pull-ups for the sleep duration. Does NOT touch
    // ADXL_INT_PIN - that stays configured as the deep-sleep wake source.
    // Call begin() again on the next wake (already done unconditionally in
    // setup(), since deep sleep re-runs it from scratch).
    void end();

private:
    bool writeReg(uint8_t reg, uint8_t value);
    uint8_t readReg(uint8_t reg);

    // Actual I2C address in use. Starts at ADXL_I2C_ADDR; begin() falls back
    // to the alternate address (0x53 <-> 0x1D, the SDO/ALT-ADDR strap) if
    // the configured one doesn't answer with the right DEVID.
    uint8_t _addr = ADXL_I2C_ADDR;
};

extern MotionSensor motionSensor;
