#pragma once
#include <Arduino.h>

// Connects to one of up to two configured WiFi networks (DeviceConfig::
// wifiSsid / wifiSsid2) - same idea as the two backends: home and office
// each have their own network. The network that connected last is tried
// first (remembered in RTC memory, so it survives deep sleep and planned
// restarts - a device living at one site doesn't pay a timeout per wake for
// the other site's network), then attempts alternate between the two until
// one connects. Non-blocking: loop() only ever issues WiFi.begin().
class WifiConnector {
public:
    // Driver must already be in STA mode. If the portal (or anything else)
    // already connected, adopts that connection instead of dropping it.
    void begin();
    // Call every loop pass while WiFi is active.
    void loop();
    // Network list changed (web UI): if unconnected, start over from the
    // preferred network right away instead of waiting out the current try.
    void restartAttempts();

    bool isConnected();
    // How long the device has been without a connection this wake cycle
    // (0 while connected).
    unsigned long unconnectedForMs();
    // 0/1 = which configured network connected most recently.
    int lastGoodSlot();
    // Every saved network was reported "not found" by the last scans.
    bool noneInRange() const { return _notFoundStreak >= 2; }

private:
    int _slot = -1; // network currently being tried/used, -1 = none
    unsigned long _attemptStartMs = 0;
    unsigned long _unconnectedSinceMs = 0;
    uint32_t _attempts = 0; // association attempts in the current offline stretch
    uint8_t _notFoundStreak = 0;      // consecutive attempts that ended "network not found"
    unsigned long _backoffUntilMs = 0; // next attempt not before this (scan back-off)
    uint32_t _attemptDisconnectBase = 0; // disconnect-event count when the attempt began
    bool _wasConnected = false;

    bool beginSlot(int slot);
    int preferredSlot();
};

extern WifiConnector wifiConnector;
