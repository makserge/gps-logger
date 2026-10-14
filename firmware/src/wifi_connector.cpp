#include "wifi_connector.h"
#include "debug_log.h"
#include "config_manager.h"
#include <WiFi.h>
#include <esp_wifi.h>

WifiConnector wifiConnector;

// Survives deep sleep AND ESP.restart() - RTC_DATA_ATTR would be reloaded
// from the image on every non-deep-sleep reset (see RtcState in main.cpp).
struct WifiRtc {
    uint32_t magic;
    int8_t lastGoodSlot;
};
static constexpr uint32_t WIFI_RTC_MAGIC = 0x57464C01u;
RTC_NOINIT_ATTR static WifiRtc s_rtc;

static int8_t &lastGood() {
    static bool checked = false;
    if (!checked) {
        checked = true;
        esp_reset_reason_t r = esp_reset_reason();
        if (s_rtc.magic != WIFI_RTC_MAGIC || r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT
                || (s_rtc.lastGoodSlot != 0 && s_rtc.lastGoodSlot != 1)) {
            s_rtc.magic = WIFI_RTC_MAGIC;
            s_rtc.lastGoodSlot = 0;
        }
    }
    return s_rtc.lastGoodSlot;
}

// Set from the WiFi event task, logged from loop() - see debug_log.h on
// why nothing may log from event callbacks directly.
static volatile uint8_t s_disconnectReason = 0;
static volatile uint32_t s_disconnectCount = 0;

static const char *slotSsid(int s) {
    auto &c = configManager.get();
    return s == 1 ? c.wifiSsid2 : c.wifiSsid;
}

static const char *slotPass(int s) {
    auto &c = configManager.get();
    return s == 1 ? c.wifiPass2 : c.wifiPass;
}

static bool slotConfigured(int s) {
    return (s == 0 || s == 1) && slotSsid(s)[0] != '\0';
}

static int configuredCount() {
    return (slotConfigured(0) ? 1 : 0) + (slotConfigured(1) ? 1 : 0);
}

// The other configured network if there is one, else the same one again,
// else -1.
static int nextSlot(int from) {
    int other = (from == 1) ? 0 : 1;
    if (slotConfigured(other)) return other;
    if (slotConfigured(from)) return from;
    return -1;
}

int WifiConnector::preferredSlot() {
    if (slotConfigured(lastGood())) return lastGood();
    return nextSlot(lastGood());
}

int WifiConnector::lastGoodSlot() { return lastGood(); }

bool WifiConnector::beginSlot(int slot) {
    if (!slotConfigured(slot)) return false;
    _slot = slot;
    _attemptStartMs = millis();
    // Abort any half-finished attempt first. Not WiFi.disconnect(): on core
    // 3.x it returns early unless already associated, leaving a running
    // scan/connect for the previous network in place - the new begin()
    // then fails with "sta is connecting".
    esp_wifi_disconnect();
    _attemptDisconnectBase = s_disconnectCount; // see failedEvent in loop()
    const char *pass = slotPass(slot);
    WiFi.begin(slotSsid(slot), pass[0] ? pass : nullptr);
    // Log the first attempts of an offline stretch, then only every 15th
    // (~5 min at 20 s each) - a long stretch off-network would otherwise
    // push all older history out of the debug log within the hour.
    _attempts++;
    if (_attempts <= 4 || _attempts % 15 == 0) {
        Log.printf("[WiFi] Trying \"%s\" (network %d of %d, attempt %lu)\n",
                   slotSsid(slot), slot + 1, configuredCount(), (unsigned long)_attempts);
    }
    return true;
}

void WifiConnector::begin() {
    // Networks live in config.json - don't let every attempt rewrite the
    // ESP-IDF's NVS copy (flash wear, and it only holds one network anyway).
    // WiFi.persistent(false) would be too late here: core 3.x only applies
    // it when the driver is initialised, which WiFi.mode() already did -
    // and that init must stay FLASH-backed so the NVS-stored network can
    // still be read for migration and by the setup portal.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    static bool eventRegistered = false;
    if (!eventRegistered) {
        WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
            s_disconnectReason = info.wifi_sta_disconnected.reason;
            s_disconnectCount = s_disconnectCount + 1;
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        eventRegistered = true;
    }
    _wasConnected = false;
    _unconnectedSinceMs = millis();
    _slot = -1;
    if (WiFi.status() == WL_CONNECTED) {
        // Already connected (captive portal just finished) - adopt it.
        String ssid = WiFi.SSID();
        for (int s = 0; s < 2; s++) {
            if (slotConfigured(s) && ssid == slotSsid(s)) _slot = s;
        }
        _attemptStartMs = millis();
        _attemptDisconnectBase = s_disconnectCount;
        return; // loop() records the connection on its next pass
    }
    int first = preferredSlot();
    if (first >= 0) beginSlot(first);
}

void WifiConnector::restartAttempts() {
    if (isConnected()) return;
    int first = preferredSlot();
    if (first >= 0) beginSlot(first);
    else _slot = -1;
}

void WifiConnector::loop() {
    unsigned long now = millis();
    wl_status_t st = WiFi.status();

    // ESP-IDF reason codes: 2 auth expired, 15 4-way handshake timeout
    // (usually wrong password), 201 no AP found (not in range / wrong
    // SSID), 202 auth fail, 8 left by us, 200 beacon timeout (signal lost).
    static uint32_t seenDisconnects = 0;
    static unsigned long lastReasonLogMs = 0;
    // At most one line per 30 s (with the count since the last line) -
    // alternating reasons (8 = our own abort, 201 = not found, 15 = bad
    // password...) otherwise logged every single attempt.
    if (s_disconnectCount != seenDisconnects) {
        uint32_t events = s_disconnectCount - seenDisconnects;
        uint8_t reason = s_disconnectReason;
        if (lastReasonLogMs == 0 || now - lastReasonLogMs >= 30000UL) {
            LOGD("[WiFi] Disconnected, last reason %u (%lu event(s), status %d, %s)\n",
                 (unsigned)reason, (unsigned long)events, (int)st, _slot >= 0 ? slotSsid(_slot) : "-");
            lastReasonLogMs = now;
            seenDisconnects = s_disconnectCount;
        }
    }

    if (st == WL_CONNECTED) {
        if (!_wasConnected) {
            _wasConnected = true;
            _unconnectedSinceMs = 0;
            _attempts = 0;
            _notFoundStreak = 0;
            _backoffUntilMs = 0;
            if (_slot >= 0) lastGood() = (int8_t)_slot;
            Log.printf("[WiFi] Connected to \"%s\", IP %s, RSSI %d dBm\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }
        return;
    }

    if (_wasConnected) {
        // Lost an established connection. The driver's auto-reconnect
        // retries the same network; the alternation below takes over if
        // it doesn't come back within one attempt window.
        _wasConnected = false;
        _unconnectedSinceMs = now;
        _attemptStartMs = now;
        // The loss event itself must not count as this retry window's
        // failure (that cut the core's auto-reconnect short after 6 s).
        _attemptDisconnectBase = s_disconnectCount;
        Log.println("[WiFi] Connection lost - retrying");
    }
    if (_unconnectedSinceMs == 0) _unconnectedSinceMs = now;

    if (_slot < 0) {
        if (_backoffUntilMs != 0) {
            if ((long)(now - _backoffUntilMs) < 0) return; // scan back-off running
            _backoffUntilMs = 0;
        }
        int s = preferredSlot(); // networks may have been added via the web UI
        if (s >= 0) beginSlot(s);
        return;
    }

    unsigned long elapsed = now - _attemptStartMs;
    // "Hopeless" = the driver already gave up on this network: status says
    // so (AP not found), or a disconnect event arrived during this attempt
    // for any reason other than 8 (ASSOC_LEAVE = our own abort in
    // beginSlot()). The event is needed for a wrong WPA2 password: core 3.x
    // reports its 4-way-handshake timeout (15/204) as WL_DISCONNECTED /
    // WL_CONNECTION_LOST, not WL_CONNECT_FAILED, so each such attempt used
    // to run the full WIFI_SLOT_TRY_MS.
    bool failedEvent = s_disconnectCount != _attemptDisconnectBase
                       && s_disconnectReason != WIFI_REASON_ASSOC_LEAVE;
    // ...but not while the driver is associated and waiting for DHCP
    // (WL_IDLE_STATUS after STA_CONNECTED): the core retries once after a
    // first failure of any reason, and that retry may be succeeding.
    bool hopeless = (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED
                     || (failedEvent && st != WL_IDLE_STATUS))
                    && elapsed >= WIFI_SLOT_FAST_FAIL_MS;
    if (elapsed >= WIFI_SLOT_TRY_MS || hopeless) {
        // "Not found" for every saved network in a row: back off between
        // scan rounds (a found-but-failed attempt resets the streak).
        if (st == WL_NO_SSID_AVAIL) {
            if (_notFoundStreak < 255) _notFoundStreak++;
        } else {
            _notFoundStreak = 0;
        }
        if (_notFoundStreak >= 2 * configuredCount() && _backoffUntilMs == 0) {
            _backoffUntilMs = now + WIFI_SCAN_BACKOFF_MS;
            esp_wifi_disconnect(); // stop the driver's own scan/retry loop meanwhile
            _slot = -1;
            return;
        }
        int next = nextSlot(_slot);
        if (next < 0 || !beginSlot(next)) _slot = -1;
    }
}

bool WifiConnector::isConnected() { return WiFi.status() == WL_CONNECTED; }

unsigned long WifiConnector::unconnectedForMs() {
    if (_unconnectedSinceMs == 0 || isConnected()) return 0;
    return millis() - _unconnectedSinceMs;
}
