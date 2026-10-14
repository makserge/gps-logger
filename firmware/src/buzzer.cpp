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

Buzzer::Pattern Buzzer::patternFor(Alert a) {
    if (a == Alert::Storage) {
        return {STORAGE_BEEP_COUNT, STORAGE_BEEP_ON_MS, STORAGE_BEEP_OFF_MS, STORAGE_BEEP_INTERVAL_MS};
    }
    return {LOW_BATTERY_BEEP_COUNT, LOW_BATTERY_BEEP_ON_MS, LOW_BATTERY_BEEP_OFF_MS, LOW_BATTERY_BEEP_INTERVAL_MS};
}

void Buzzer::startPattern() {
    _beepsRemaining = patternFor(_alert).count;
    _state = State::BeepOn;
    _stateChangedAt = millis();
    _patternRunning = true;
    setBuzzer(true);
}

void Buzzer::stop() {
    if (_state != State::Idle) {
        _state = State::Idle;
        setBuzzer(false);
    }
    _patternRunning = false;
}

void Buzzer::update(bool lowBattery, bool storageAlert) {
    unsigned long now = millis();
    Alert wanted = lowBattery ? Alert::LowBattery : (storageAlert ? Alert::Storage : Alert::None);

    if (wanted != _alert) {
        // Condition cleared, or a different alert took over - stop at once,
        // mid-pattern or not; a new alert starts its pattern right away.
        stop();
        _alert = wanted;
    }
    if (_alert == Alert::None) return;

    Pattern p = patternFor(_alert);

    // Kick off a new pattern if idle and the repeat interval has elapsed.
    if (_state == State::Idle) {
        if (!_patternRunning || (now - _lastPatternStart) >= p.intervalMs) {
            _lastPatternStart = now;
            startPattern();
        }
        return;
    }

    // Advance the beep on/off state machine.
    if (_state == State::BeepOn && (now - _stateChangedAt) >= p.onMs) {
        setBuzzer(false);
        _beepsRemaining--;
        _stateChangedAt = now;
        _state = (_beepsRemaining > 0) ? State::BeepOff : State::Idle;
    } else if (_state == State::BeepOff && (now - _stateChangedAt) >= p.offMs) {
        setBuzzer(true);
        _stateChangedAt = now;
        _state = State::BeepOn;
    }
}
