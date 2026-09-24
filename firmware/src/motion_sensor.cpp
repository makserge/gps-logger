#include "motion_sensor.h"
#include <Wire.h>
#include <esp_log.h>

MotionSensor motionSensor;

// ADXL345 register map (only what we need)
#define REG_THRESH_ACT   0x24
#define REG_THRESH_INACT 0x25
#define REG_TIME_INACT   0x26
#define REG_ACT_INACT_CTL 0x27
#define REG_BW_RATE      0x2C
#define REG_POWER_CTL    0x2D
#define REG_INT_ENABLE   0x2E
#define REG_INT_MAP      0x2F
#define REG_INT_SOURCE   0x30
#define REG_DATA_FORMAT  0x31
#define REG_DEVID        0x00

// ACT_INACT_CTL (0x27) bit layout: [7]=ACT ac/dc, [6:4]=ACT X/Y/Z enable,
// [3]=INACT ac/dc, [2:0]=INACT X/Y/Z enable. The ac/dc bits are CRITICAL:
// DC-coupled compares the raw sample - gravity included, ~1000mg on some
// axis at rest - against the threshold, so any threshold below 1g fires
// continuously on a motionless board. (0x70, used here previously, was
// exactly that bug: X/Y/Z enabled but bit7 clear = DC-coupled, giving a
// permanent activity interrupt that reset the inactivity clock every loop
// pass - the device could never go to sleep.) AC-coupled subtracts a
// reference sample first, so only CHANGES in acceleration count.
#define ACT_INACT_CTL_AC_XYZ_BOTH 0xFF // AC-coupled activity + inactivity, X/Y/Z enabled on both
#define DATA_FORMAT_INT_INVERT   0x20 // INT1/INT2 active-low instead of active-high
#define INT_ENABLE_ACTIVITY      0x10
#define INT_ENABLE_INACTIVITY    0x08
#define INT_MAP_INACT_TO_INT2    0x08 // inactivity -> INT2 (unconnected); activity stays on INT1
#define POWER_CTL_MEASURE        0x08
#define POWER_CTL_LINK           0x20 // serialize activity/inactivity (see begin())

bool MotionSensor::writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

uint8_t MotionSensor::readReg(uint8_t reg) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    Wire.requestFrom((int)_addr, 1);
    if (Wire.available()) return Wire.read();
    return 0;
}

bool MotionSensor::begin() {
    // Silence the IDF I2C driver's own error logging (e.g. "I2C transaction
    // unexpected nack detected") - a missing/unwired ADXL345 is an expected,
    // handled case here (see the DEVID check below), and we already print
    // our own clear one-line message for it, so the raw driver spam is just
    // noise that can look alarming (it is NOT a crash).
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    Wire.begin(ADXL_SDA_PIN, ADXL_SCL_PIN);
    // Bound every I2C transaction to 50ms. Without this, a missing or
    // miswired ADXL345 (floating/stuck SDA/SCL) can hang Wire.begin()'s
    // subsequent transactions indefinitely - a known ESP32 I2C driver
    // failure mode - which would silently freeze the *entire* boot before
    // WiFi/the captive portal ever runs, since this is called early in
    // setup(). With the timeout, a bad sensor just fails begin() below and
    // the rest of the device (WiFi, logging, everything) still boots fine.
    Wire.setTimeOut(50);
    pinMode(ADXL_INT_PIN, INPUT_PULLUP); // idles high, pulses low on activity (INT_INVERT)

    _addr = ADXL_I2C_ADDR;
    uint8_t id = readReg(REG_DEVID);
    if (id != 0xE5) {
        // Not answering at the configured address - try the other one before
        // giving up. 0x53 vs 0x1D is just the SDO/ALT-ADDR strap pin, an easy
        // thing to have wired (or for a breakout to strap) the other way.
        uint8_t altAddr = (ADXL_I2C_ADDR == 0x53) ? 0x1D : 0x53;
        _addr = altAddr;
        uint8_t altId = readReg(REG_DEVID);
        if (altId != 0xE5) {
            Serial.printf("[Motion] ADXL345 not found (DEVID=0x%02X @0x%02X, 0x%02X @0x%02X), motion wake disabled\n",
                          id, ADXL_I2C_ADDR, altId, altAddr);
            _addr = ADXL_I2C_ADDR;
            return false;
        }
        Serial.printf("[Motion] ADXL345 answered at ALTERNATE address 0x%02X, not the configured 0x%02X - "
                      "SDO/ALT-ADDR is strapped the other way; set ADXL_I2C_ADDR to match\n",
                      altAddr, ADXL_I2C_ADDR);
    }

    // Standby while configuring
    writeReg(REG_POWER_CTL, 0x00);

    // Activity/inactivity thresholds: registers are in 62.5 mg/LSB steps
    uint8_t threshLsb = (uint8_t)constrain(ADXL_ACT_THRESHOLD_MG / 62.5f, 1, 255);
    writeReg(REG_THRESH_ACT, threshLsb);
    uint8_t inactThreshLsb = (uint8_t)constrain(ADXL_INACT_THRESHOLD_MG / 62.5f, 1, 255);
    writeReg(REG_THRESH_INACT, inactThreshLsb);
    writeReg(REG_TIME_INACT, ADXL_INACT_TIME_S); // 1 s/LSB

    // Inactivity detection + the LINK bit exist purely to keep AC-coupled
    // activity detection honest: the AC reference sample is captured when
    // activity detection (re)starts, so without re-arming, a permanent
    // orientation change (device flipped over and left there) reads as a
    // constant >threshold "difference" and fires forever - same never-sleeps
    // symptom as the DC bug above, just rarer. With LINK set, activity and
    // inactivity run alternately: activity fires -> inactivity detection
    // waits for ADXL_INACT_TIME_S of stillness -> activity re-arms with a
    // FRESH reference for whatever the resting orientation now is.
    writeReg(REG_ACT_INACT_CTL, ACT_INACT_CTL_AC_XYZ_BOTH);
    writeReg(REG_DATA_FORMAT, DATA_FORMAT_INT_INVERT); // +-2g range, active-low INT
    // Inactivity goes to INT2 (unconnected) - it must NOT share INT1, or the
    // "3s of stillness" event firing shortly after each deep sleep entry
    // would pull INT1 low and instant-wake the chip.
    writeReg(REG_INT_MAP, INT_MAP_INACT_TO_INT2);
    writeReg(REG_BW_RATE, 0x0A);      // 100 Hz output rate, normal power mode
    // Inactivity must be interrupt-enabled for its function (and thus LINK
    // re-arming) to run, even though nothing listens on INT2.
    writeReg(REG_INT_ENABLE, INT_ENABLE_ACTIVITY | INT_ENABLE_INACTIVITY);

    // Enter measurement mode with LINK (must be set together, while coming
    // out of standby, for the linked state machine to start cleanly)
    writeReg(REG_POWER_CTL, POWER_CTL_LINK | POWER_CTL_MEASURE);

    clearInterrupt(); // drop any stale latched interrupt from config writes

    // Sanity-check the physical INT1 line: with INT_INVERT set and the latch
    // just cleared, the pin must idle HIGH (a missing wire also reads HIGH
    // via the pull-up - that case is caught by never seeing activity
    // interrupts instead). Something actively driving it LOW here (short,
    // wrong pin, INT2 wired instead of INT1) means motion wake can't work,
    // and enterDeepSleep()'s guard will skip arming it.
    if (digitalRead(ADXL_INT_PIN) == LOW) {
        Serial.println("[Motion] WARNING: INT1 reads LOW after config+clear - check the INT1 wire to D2; motion wake will be skipped at sleep");
    }

    Serial.printf("[Motion] ADXL345 @0x%02X configured for activity-interrupt wake\n", _addr);
    return true;
}

uint8_t MotionSensor::readAndClearIntSource() {
    return readReg(REG_INT_SOURCE); // reading this register de-latches INT1/INT2
}

void MotionSensor::clearInterrupt() {
    readAndClearIntSource();
}

bool MotionSensor::readAccel(float &xMg, float &yMg, float &zMg) {
    // Burst-read DATAX0..DATAZ1 (0x32-0x37) in one transaction, as the
    // datasheet requires for consistent multi-byte samples.
    Wire.beginTransmission(_addr);
    Wire.write(0x32);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)_addr, 6) != 6) return false;
    int16_t raw[3];
    for (int i = 0; i < 3; i++) {
        uint8_t lo = Wire.read();
        uint8_t hi = Wire.read();
        raw[i] = (int16_t)((hi << 8) | lo);
    }
    const float mgPerLsb = 3.9f; // 10-bit, +-2g
    xMg = raw[0] * mgPerLsb;
    yMg = raw[1] * mgPerLsb;
    zMg = raw[2] * mgPerLsb;
    return true;
}

uint64_t MotionSensor::wakeupPinMask() {
    return 1ULL << ADXL_INT_PIN;
}

void MotionSensor::end() {
    Wire.end();
    pinMode(ADXL_SDA_PIN, INPUT);
    pinMode(ADXL_SCL_PIN, INPUT);
    // ADXL_INT_PIN deliberately untouched - PowerManager::enterDeepSleep()
    // configures it as the GPIO wakeup source right after this runs.
}
