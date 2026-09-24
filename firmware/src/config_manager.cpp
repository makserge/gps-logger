#include "config_manager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <WiFi.h>
#include <time.h>

ConfigManager configManager;

void ConfigManager::applyDefaults() {
    memset(&_cfg, 0, sizeof(_cfg));
    strlcpy(_cfg.backendHost, "homeassistant.local", sizeof(_cfg.backendHost));
    _cfg.backendPort = 8080;
    _cfg.otaEnabled = true;
    strlcpy(_cfg.tzPosix, "UTC0", sizeof(_cfg.tzPosix));
}

bool ConfigManager::begin() {
    applyDefaults();
    if (!LittleFS.exists(FS_CONFIG_PATH)) {
        applyTimezone();
        return false;
    }

    File f = LittleFS.open(FS_CONFIG_PATH, "r");
    if (!f) {
        applyTimezone();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[Config] parse error: %s\n", err.c_str());
        applyTimezone();
        return false;
    }

    strlcpy(_cfg.backendHost, doc["backendHost"] | "homeassistant.local", sizeof(_cfg.backendHost));
    _cfg.backendPort = doc["backendPort"] | 8080;
    strlcpy(_cfg.deviceId, doc["deviceId"] | "", sizeof(_cfg.deviceId));
    _cfg.otaEnabled = doc["otaEnabled"] | true;
    strlcpy(_cfg.tzPosix, doc["tzPosix"] | "UTC0", sizeof(_cfg.tzPosix));
    applyTimezone();
    return true;
}

bool ConfigManager::save() {
    JsonDocument doc;
    doc["backendHost"] = _cfg.backendHost;
    doc["backendPort"] = _cfg.backendPort;
    doc["deviceId"] = _cfg.deviceId;
    doc["otaEnabled"] = _cfg.otaEnabled;
    doc["tzPosix"] = _cfg.tzPosix;

    File f = LittleFS.open(FS_CONFIG_PATH, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

void ConfigManager::applyTimezone() {
    // setenv+tzset is how the C library learns the local offset AND DST
    // transition rule from a POSIX TZ string - every subsequent
    // localtime_r() call anywhere in the firmware automatically reflects
    // it, including DST switching itself on/off at the right moments, with
    // no custom date-math needed on our part.
    setenv("TZ", _cfg.tzPosix, 1);
    tzset();
}

bool ConfigManager::setTimezone(const String &posixTz) {
    if (posixTz.length() == 0 || posixTz.length() >= sizeof(_cfg.tzPosix)) return false;
    strlcpy(_cfg.tzPosix, posixTz.c_str(), sizeof(_cfg.tzPosix));
    applyTimezone();
    return save();
}

String ConfigManager::deviceId() {
    if (strlen(_cfg.deviceId) > 0) return String(_cfg.deviceId);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "gpslog-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return String(buf);
}

bool ConfigManager::isWifiConfigured() {
    return WiFi.SSID().length() > 0;
}

// Custom WiFiManager parameters, wired to _cfg on save.
void ConfigManager::runCaptivePortal() {
    WiFi.mode(WIFI_STA); // explicit, clean starting state before WiFiManager takes over

    WiFiManager wm;
    // Verbose *wm: logging on Serial - this is the single most useful thing
    // for diagnosing "enters valid credentials but won't connect": it shows
    // the actual ESP-IDF connect error (e.g. wrong password, AP not found,
    // security-mode mismatch) instead of just a silent pass/fail.
    wm.setDebugOutput(true);

    WiFiManagerParameter p_host("host", "Backend host/IP", _cfg.backendHost, sizeof(_cfg.backendHost) - 1);
    char portBuf[8]; snprintf(portBuf, sizeof(portBuf), "%u", _cfg.backendPort);
    WiFiManagerParameter p_port("port", "Backend REST port", portBuf, 6);
    wm.addParameter(&p_host);
    wm.addParameter(&p_port);

    wm.setConfigPortalTimeout(180); // give up after 3 min, retry later
    wm.setBreakAfterConfig(true);
    wm.setConnectRetries(3);        // retry a flaky first attempt before giving up
    wm.setConnectTimeout(15);       // seconds per attempt

    String apName = "GPSLogger-Setup-" + deviceId().substring(deviceId().length() - 4);

    // autoConnect() tries the last-known/saved credentials first and only
    // falls back to opening the portal AP if that fails - which is exactly
    // what's wanted here, since this is only ever called when no WiFi is
    // configured yet at all (see main.cpp).
    bool ok = wm.autoConnect(apName.c_str());

    if (ok) {
        strlcpy(_cfg.backendHost, p_host.getValue(), sizeof(_cfg.backendHost));
        _cfg.backendPort = atoi(p_port.getValue());
        if (strlen(_cfg.deviceId) == 0) {
            strlcpy(_cfg.deviceId, deviceId().c_str(), sizeof(_cfg.deviceId));
        }
        save();
        Serial.println("[Config] WiFi + backend config saved");

        // WiFiManager returning true means the portal flow completed and it
        // believes it connected - but a bad password, WPA3-only/PMF-required
        // security, or a 5GHz-only SSID can all cause it to accept the
        // credentials and still fail to actually associate. Verify for real.
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
            delay(250);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[Config] WiFi connected, IP: %s, RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            Serial.printf(
                "[Config] WARNING: WiFi did not actually connect (status=%d). "
                "Common causes: the router is 5GHz-only or band-steering "
                "(the ESP32-C6 only supports 2.4GHz - try a dedicated "
                "2.4GHz-only SSID), WPA3-only/PMF-required security (try "
                "WPA2/WPA3-mixed instead), or a mistyped password. The "
                "device will keep retrying in the background.\n",
                (int)WiFi.status());
        }
    } else {
        Serial.println("[Config] Config portal failed/timed out, continuing offline");
        // If saved credentials DO exist (portal was reached because a
        // blocking connect attempt failed, not because the device is
        // unconfigured), don't stay offline for the whole wake cycle -
        // hand the connection back to the WiFi stack, which retries in
        // the background. A truly unconfigured device has nothing saved
        // and this begin() is a harmless no-op failure.
        WiFi.mode(WIFI_STA);
        WiFi.begin();
    }
}
