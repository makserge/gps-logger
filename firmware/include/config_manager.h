#pragma once
#include "config.h"

class ConfigManager {
public:
    bool begin();                     // load from LittleFS, or defaults
    bool save();                      // persist to LittleFS
    void runCaptivePortal();          // blocks until WiFi + params configured
    bool isWifiConfigured();

    // Sets _cfg.tzPosix, applies it immediately (setenv+tzset - takes
    // effect for the very next localtime_r() call, no reboot needed), and
    // persists it. Returns false without changing anything if the string
    // is empty or too long for DeviceConfig::tzPosix.
    bool setTimezone(const String &posixTz);

    // Re-applies the already-stored tzPosix (setenv+tzset) without
    // changing or persisting anything. Needed after calling Arduino-ESP32's
    // configTime() (used for NTP - see SyncManager), which has the side
    // effect of setting its own TZ from the offsets passed to it,
    // clobbering whatever TZ was set before it - call this right after to
    // restore the configured zone.
    void applyTimezone();

    DeviceConfig &get() { return _cfg; }
    String deviceId();                // stable id derived from MAC if not set

private:
    DeviceConfig _cfg{};
    void applyDefaults();
};

extern ConfigManager configManager;
