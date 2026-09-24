#pragma once
#include "config.h"

// Drives a simple beeper on low battery: 5 short beeps, repeating once a
// minute, non-blocking (state machine driven from loop()) so it never stalls
// GPS parsing, WiFi/MQTT, or the web server.
class Buzzer {
public:
    void begin();

    // Call every loop() iteration. lowBattery reflects the *current* state
    // (PowerManager::isLow() && !isCritical()); pattern stops as soon as
    // this goes false (e.g. battery recovers via charging).
    void update(bool lowBattery);

private:
    enum class State { Idle, BeepOn, BeepOff };
    State _state = State::Idle;
    uint8_t _beepsRemaining = 0;
    unsigned long _stateChangedAt = 0;
    unsigned long _lastPatternStart = 0;
    bool _patternRunning = false;

    void startPattern();
    void setBuzzer(bool on);
};

extern Buzzer buzzer;
