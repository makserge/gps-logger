#pragma once
#include "config.h"

class ConfigManager {
public:
    bool begin();                     // load from LittleFS, or defaults
    bool save();                      // persist to LittleFS
    // Blocks (up to 3 min) serving the setup AP. On success the network it
    // connected to is ADDED to the two WiFi slots: an existing entry with
    // the same SSID gets the new password, else an empty slot is used,
    // else slot replaceSlotIfFull is overwritten (callers pass the network
    // that did NOT connect most recently). Returns true if connected.
    bool runCaptivePortal(int replaceSlotIfFull = 1);
    bool isWifiConfigured();          // at least one network stored in config
    // One-time import of the network the ESP-IDF saved in NVS (portal
    // setups before networks moved to config.json). Driver must be in STA
    // mode. No-op once DeviceConfig::wifiMigrated is set.
    void migrateLegacyWifi();
    // Empties both network slots (web UI "Reset WiFi") and persists.
    void clearWifiNetworks();
    // Slot placement shared by migration and the portal (see runCaptivePortal).
    void addWifiNetwork(const String &ssid, const String &pass, int replaceSlotIfFull);

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
