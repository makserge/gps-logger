#include "config_manager.h"
#include "debug_log.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>

ConfigManager configManager;

void ConfigManager::applyDefaults() {
    memset(&_cfg, 0, sizeof(_cfg));
    strlcpy(_cfg.backendHost, "homeassistant.local", sizeof(_cfg.backendHost));
    _cfg.backendPort = 8080;
    _cfg.backendHost2[0] = '\0';
    _cfg.backendPort2 = 8080;
    _cfg.reclaimSynced = true;
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
        Log.printf("[Config] parse error: %s\n", err.c_str());
        applyTimezone();
        return false;
    }

    strlcpy(_cfg.backendHost, doc["backendHost"] | "homeassistant.local", sizeof(_cfg.backendHost));
    _cfg.backendPort = doc["backendPort"] | 8080;
    strlcpy(_cfg.backendHost2, doc["backendHost2"] | "", sizeof(_cfg.backendHost2));
    _cfg.backendPort2 = doc["backendPort2"] | 8080;
    strlcpy(_cfg.wifiSsid, doc["wifiSsid"] | "", sizeof(_cfg.wifiSsid));
    strlcpy(_cfg.wifiPass, doc["wifiPass"] | "", sizeof(_cfg.wifiPass));
    strlcpy(_cfg.wifiSsid2, doc["wifiSsid2"] | "", sizeof(_cfg.wifiSsid2));
    strlcpy(_cfg.wifiPass2, doc["wifiPass2"] | "", sizeof(_cfg.wifiPass2));
    _cfg.wifiMigrated = doc["wifiMigrated"] | false;
    _cfg.debugLog = doc["debugLog"] | false;
    _cfg.reclaimSynced = doc["reclaimSynced"] | true;
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
    doc["backendHost2"] = _cfg.backendHost2;
    doc["backendPort2"] = _cfg.backendPort2;
    doc["wifiSsid"] = _cfg.wifiSsid;
    doc["wifiPass"] = _cfg.wifiPass;
    doc["wifiSsid2"] = _cfg.wifiSsid2;
    doc["wifiPass2"] = _cfg.wifiPass2;
    doc["wifiMigrated"] = _cfg.wifiMigrated;
    doc["debugLog"] = _cfg.debugLog;
    doc["reclaimSynced"] = _cfg.reclaimSynced;
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
    return _cfg.wifiSsid[0] != '\0' || _cfg.wifiSsid2[0] != '\0';
}

// The network the ESP-IDF driver has stored (NVS-backed station config).
// Deliberately NOT WiFi.SSID(): on core 3.x that returns the SSID of the AP
// currently associated with, which is empty before anything connected.
static bool readDriverStoredNetwork(String &ssid, String &pass) {
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return false;
    char s[33] = {0};
    char p[65] = {0};
    memcpy(s, conf.sta.ssid, 32);      // not necessarily NUL-terminated at 32
    memcpy(p, conf.sta.password, 64);
    ssid = s;
    pass = p;
    return true;
}

// Puts a network into a slot: an existing entry with the same SSID gets the
// new password, else an empty slot is used, else replaceSlotIfFull.
void ConfigManager::addWifiNetwork(const String &ssid, const String &pass, int replaceSlotIfFull) {
    if (ssid.length() == 0 || ssid.length() >= sizeof(_cfg.wifiSsid)) return;
    int slot;
    if (ssid == _cfg.wifiSsid) slot = 0;
    else if (ssid == _cfg.wifiSsid2) slot = 1;
    else if (_cfg.wifiSsid[0] == '\0') slot = 0;
    else if (_cfg.wifiSsid2[0] == '\0') slot = 1;
    else slot = (replaceSlotIfFull == 0) ? 0 : 1;
    strlcpy(slot == 0 ? _cfg.wifiSsid : _cfg.wifiSsid2, ssid.c_str(), sizeof(_cfg.wifiSsid));
    strlcpy(slot == 0 ? _cfg.wifiPass : _cfg.wifiPass2, pass.c_str(), sizeof(_cfg.wifiPass));
    Log.printf("[Config] WiFi network \"%s\" saved as network %d\n", ssid.c_str(), slot + 1);
}

void ConfigManager::migrateLegacyWifi() {
    if (_cfg.wifiMigrated) return;
    if (!isWifiConfigured()) {
        String ssid, pass;
        if (!readDriverStoredNetwork(ssid, pass)) {
            Log.println("[Config] Could not read the driver's stored WiFi config - will retry next boot");
            return; // not marked migrated: try again next time
        }
        if (ssid.length() > 0) {
            addWifiNetwork(ssid, pass, 1);
            Log.printf("[Config] Imported saved WiFi network \"%s\" as primary network\n", ssid.c_str());
        }
    }
    _cfg.wifiMigrated = true;
    save();
}

void ConfigManager::clearWifiNetworks() {
    memset(_cfg.wifiSsid, 0, sizeof(_cfg.wifiSsid));
    memset(_cfg.wifiPass, 0, sizeof(_cfg.wifiPass));
    memset(_cfg.wifiSsid2, 0, sizeof(_cfg.wifiSsid2));
    memset(_cfg.wifiPass2, 0, sizeof(_cfg.wifiPass2));
    _cfg.wifiMigrated = true;
    save();
}

// Custom WiFiManager parameters, wired to _cfg on save.
bool ConfigManager::runCaptivePortal(int replaceSlotIfFull) {
    WiFi.mode(WIFI_STA); // explicit, clean starting state before WiFiManager takes over

    WiFiManager wm(Log); // its *wm: debug output goes through the debug log too
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
    // Stay in the portal after a failed connect (typo in the password) so
    // the user can simply retry - breaking out closed the AP after one
    // attempt. Only a successful connect or the timeout ends it.
    wm.setBreakAfterConfig(false);
    // No firmware update (or config erase) through the portal: the AP is
    // open (no password), and anyone in range during those 3 minutes could
    // flash arbitrary firmware through it. setMenu() only hides the
    // buttons - WiFiManager still registers /update, /u and /erase. Its web
    // server callback runs BEFORE it registers its own routes, and
    // WebServer uses the first matching handler, so these win. OTA stays
    // available on the device's own UI.
    std::vector<const char *> menu = {"wifi", "exit"};
    wm.setMenu(menu);
    wm.setWebServerCallback([&wm]() {
        auto refuse = [&wm]() { wm.server->send(403, "text/plain", "disabled"); };
        wm.server->on("/u", HTTP_POST, refuse, []() {}); // empty upload handler: image bytes are ignored
        wm.server->on("/update", refuse);
        wm.server->on("/erase", refuse);
    });
    wm.setConnectRetries(3);        // retry a flaky first attempt before giving up
    wm.setConnectTimeout(15);       // seconds per attempt

    String apName = "GPSLogger-Setup-" + deviceId().substring(deviceId().length() - 4);

    // startConfigPortal(), not autoConnect(): this only runs when no stored
    // network is configured or none of them connected (see main.cpp), so
    // re-trying the NVS copy first would just add ~45s before the AP opens.
    bool ok = wm.startConfigPortal(apName.c_str());

    if (ok) {
        const char *host = p_host.getValue();
        if (host[0] != '\0') strlcpy(_cfg.backendHost, host, sizeof(_cfg.backendHost));
        long port = atol(p_port.getValue());
        if (port >= 1 && port <= 65535) _cfg.backendPort = (uint16_t)port;
        if (strlen(_cfg.deviceId) == 0) {
            strlcpy(_cfg.deviceId, deviceId().c_str(), sizeof(_cfg.deviceId));
        }
        _cfg.wifiMigrated = true;
        save();
        Log.println("[Config] Backend config saved");

        // WiFiManager returning true means the portal flow completed and it
        // believes it connected - but a bad password, WPA3-only/PMF-required
        // security, or a 5GHz-only SSID can all cause it to accept the
        // credentials and still fail to actually associate. Verify for real.
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
            delay(250);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Log.printf("[Config] WiFi connected, IP: %s, RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            Log.printf(
                "[Config] WARNING: WiFi did not actually connect (status=%d). "
                "Common causes: the router is 5GHz-only or band-steering "
                "(the ESP32-C6 only supports 2.4GHz - try a dedicated "
                "2.4GHz-only SSID), WPA3-only/PMF-required security (try "
                "WPA2/WPA3-mixed instead), or a mistyped password. The "
                "device will keep retrying in the background.\n",
                (int)WiFi.status());
        }
    } else {
        // The caller hands the stored networks back to WifiConnector,
        // which keeps retrying them in the background.
        Log.println("[Config] Config portal failed/timed out, continuing offline");
        WiFi.mode(WIFI_STA);
    }
    // Save the network the portal connected to - ONLY on success. The
    // driver's stored config after a timeout is whatever NVS held (often a
    // stale copy of a network whose password was changed in Settings since,
    // which WifiConnector never writes to NVS), and after a failed attempt
    // it holds the mistyped credentials - importing either overwrote a
    // working slot and locked the device out of its own network.
    if (ok && WiFi.status() == WL_CONNECTED) {
        addWifiNetwork(WiFi.SSID(), WiFi.psk(), replaceSlotIfFull);
        _cfg.wifiMigrated = true;
        save();
    }
    return ok;
}
