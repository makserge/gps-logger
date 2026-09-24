#include "buzzer.h"
#include <Arduino.h>

Buzzer buzzer;

void Buzzer::begin() {
    pinMode(BUZZER_PIN, OUTPUT);
    setBuzzer(false);
}

void Buzzer::setBuzzer(bool on) {
    digitalWrite(BUZZER_PIN, on ? HIGH : LOW); // assumes active-high buzzer/driver transistor
}

void Buzzer::startPattern() {
    _beepsRemaining = LOW_BATTERY_BEEP_COUNT;
    _state = State::BeepOn;
    _stateChangedAt = millis();
    _patternRunning = true;
    setBuzzer(true);
}

void Buzzer::update(bool lowBattery) {
    unsigned long now = millis();

    if (!lowBattery) {
        // Battery recovered (e.g. now charging) - stop immediately, mid-pattern or not.
        if (_state != State::Idle) {
            _state = State::Idle;
            setBuzzer(false);
        }
        _patternRunning = false;
        return;
    }

    // Kick off a new pattern if idle and the repeat interval has elapsed.
    if (_state == State::Idle) {
        if (!_patternRunning || (now - _lastPatternStart) >= LOW_BATTERY_BEEP_INTERVAL_MS) {
            _lastPatternStart = now;
            startPattern();
        }
        return;
    }

    // Advance the beep on/off state machine.
    if (_state == State::BeepOn && (now - _stateChangedAt) >= LOW_BATTERY_BEEP_ON_MS) {
        setBuzzer(false);
        _beepsRemaining--;
        _stateChangedAt = now;
        _state = (_beepsRemaining > 0) ? State::BeepOff : State::Idle;
    } else if (_state == State::BeepOff && (now - _stateChangedAt) >= LOW_BATTERY_BEEP_OFF_MS) {
        setBuzzer(true);
        _stateChangedAt = now;
        _state = State::BeepOn;
    }
}
