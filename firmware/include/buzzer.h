#pragma once
#include "config.h"

// Drives a simple beeper, non-blocking (state machine driven from loop())
// so it never stalls GPS parsing, WiFi or the web server. Two alerts with
// distinct patterns, low battery taking precedence:
//   - low battery:  LOW_BATTERY_BEEP_COUNT short beeps, once a minute
//   - storage full: STORAGE_BEEP_COUNT long beeps, every STORAGE_BEEP_INTERVAL_MS
//     (the caller only asks for it in daytime - see main.cpp)
class Buzzer {
public:
    void begin();

    // Call every loop() iteration with the *current* conditions; a pattern
    // stops as soon as its condition goes false (e.g. charging, or tracks
    // uploaded and space freed).
    void update(bool lowBattery, bool storageAlert = false);
    // A beep (or the gap inside a pattern) is in progress - loop() must keep
    // running to time it, so no light sleep now.
    bool busy() const { return _state != State::Idle; }

private:
    enum class Alert { None, LowBattery, Storage };
    enum class State { Idle, BeepOn, BeepOff };
    struct Pattern {
        uint8_t count;
        unsigned long onMs, offMs, intervalMs;
    };
    State _state = State::Idle;
    Alert _alert = Alert::None;
    uint8_t _beepsRemaining = 0;
    unsigned long _stateChangedAt = 0;
    unsigned long _lastPatternStart = 0;
    bool _patternRunning = false;

    static Pattern patternFor(Alert a);
    void startPattern();
    void stop();
    void setBuzzer(bool on);
};

extern Buzzer buzzer;
