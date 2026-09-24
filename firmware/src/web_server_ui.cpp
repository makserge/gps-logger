#include "web_server_ui.h"
#include "track_store.h"
#include "power_manager.h"
#include "sync_manager.h"
#include "config_manager.h"
#include "gps_reader.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFi.h>
#include <time.h>

WebUi webUi;

// Server-side OTA state, polled by the web UI so it can confirm the flash
// actually succeeded (and show a real error) rather than just assuming
// success once the upload finishes.
static volatile bool otaActive = false;
static volatile size_t otaWrittenBytes = 0;
static String otaLastError = "";
static bool otaLastSuccess = false;

// Minimal, dependency-free HTML/JS UI: calendar list of dates -> sessions ->
// download, plus an OTA firmware flash panel with upload/flash progress.
// Kept intentionally simple per requirements ("simplified only" viewer).
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GPS Logger</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:1rem;background:#111;color:#eee}
h1{font-size:1.2rem} .card{background:#1c1c1c;border-radius:8px;padding:1rem;margin-bottom:.75rem}
.date{font-weight:600;margin-bottom:.4rem;cursor:pointer}
.date-block{margin-bottom:1rem} .date-block:last-child{margin-bottom:0}
.section-title{font-weight:600;margin-bottom:.6rem}
.session{display:flex;justify-content:space-between;padding:.4rem 0;border-top:1px solid #333}
a.btn{color:#4ea3ff;text-decoration:none} .status{font-size:.85rem;color:#9a9}
button{background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:6px;padding:.5rem .8rem}
button:disabled{opacity:.5}
input[type=file]{color:#eee;font-size:.85rem}
.progress-track{background:#2a2a2a;border-radius:4px;height:10px;overflow:hidden;margin-top:.6rem}
.progress-bar{background:#4ea3ff;height:100%;width:0%;transition:width .2s}
.progress-bar.error{background:#e05a5a}
.sat-table{width:100%;border-collapse:collapse;font-size:.8rem}
.sat-table th,.sat-table td{padding:.3rem .4rem;text-align:left;border-top:1px solid #333}
.sat-bar{background:#333;border-radius:3px;overflow:hidden;width:60px;height:8px;display:inline-block}
.sat-bar-fill{height:100%}
</style></head><body>
<h1>GPS Logger</h1>
<div class="card">
  <div class="status" id="status">loading status...</div>
  <div class="status" id="statusTime" style="margin-top:.3rem"></div>
  <div class="status" id="statusSync" style="margin-top:.3rem"></div>
  <div style="margin-top:.6rem">
    <button onclick="syncNow()">Sync now</button>
    <a class="btn" href="#" id="resetWifiLink" onclick="resetWifi(); return false;" style="margin-left:.8rem">Reset WiFi</a>
    <a class="btn" href="#" onclick="changeBackend(); return false;" style="margin-left:.8rem">Backend...</a>
    <span style="margin-left:.8rem;font-size:.85rem">
      Timezone: <select id="tzSelect" onchange="setTimezone()"></select>
    </span>
  </div>
</div>
<div class="card">
  <div class="section-title">Satellites</div>
  <div class="status" id="satSummary">loading...</div>
  <table class="sat-table">
    <thead><tr><th>Satellite</th><th>Elev</th><th>Az</th><th>Signal</th><th></th></tr></thead>
    <tbody id="satBody"></tbody>
  </table>
</div>
<div class="card">
  <div class="section-title">Firmware update (OTA)</div>
  <input type="file" id="otaFile" accept=".bin">
  <button id="otaBtn" onclick="flashFirmware()">Flash</button>
  <div class="progress-track"><div class="progress-bar" id="otaBar"></div></div>
  <div class="status" id="otaLabel">Select a firmware.bin file to flash over WiFi.</div>
</div>
<div class="card">
  <div class="section-title">Tracks</div>
  <div id="calendar">loading sessions...</div>
</div>
<script>
let tzOptionsBuilt = false;
let tzUserIsEditing = false;
// A curated set of common zones as POSIX TZ strings, which encode both the
// base UTC offset AND the DST transition rule (start/end dates, amount) in
// one string - the C library's localtime_r() handles switching itself on
// and off automatically from this, no manual twice-a-year toggling needed.
const TZ_PRESETS = [
  ['UTC0', 'UTC'],
  ['GMT0BST,M3.5.0/1,M10.5.0', 'UK (London)'],
  ['WET0WEST,M3.5.0/1,M10.5.0', 'Western Europe (Lisbon)'],
  ['CET-1CEST,M3.5.0,M10.5.0/3', 'Central Europe (Berlin/Paris/Rome/Madrid)'],
  ['EET-2EEST,M3.5.0/3,M10.5.0/4', 'Eastern Europe (Helsinki/Athens/Kyiv)'],
  ['MSK-3', 'Moscow'],
  ['EST5EDT,M3.2.0,M11.1.0', 'US Eastern (New York)'],
  ['CST6CDT,M3.2.0,M11.1.0', 'US Central (Chicago)'],
  ['MST7MDT,M3.2.0,M11.1.0', 'US Mountain (Denver)'],
  ['PST8PDT,M3.2.0,M11.1.0', 'US Pacific (Los Angeles)'],
  ['IST-5:30', 'India'],
  ['CST-8', 'China (Beijing)'],
  ['JST-9', 'Japan (Tokyo)'],
  ['AEST-10AEDT,M10.1.0,M4.1.0/3', 'Australia Eastern (Sydney)'],
  ['NZST-12NZDT,M9.5.0,M4.1.0/3', 'New Zealand (Auckland)'],
];
function buildTimezoneOptions(){
  const sel = document.getElementById('tzSelect');
  TZ_PRESETS.forEach(([tz, label]) => {
    const opt = document.createElement('option');
    opt.value = tz;
    opt.innerText = label;
    sel.appendChild(opt);
  });
  const customOpt = document.createElement('option');
  customOpt.value = '__custom__';
  customOpt.innerText = 'Custom POSIX TZ string...';
  sel.appendChild(customOpt);
  sel.addEventListener('focus', () => tzUserIsEditing = true);
  sel.addEventListener('blur', () => tzUserIsEditing = false);
  tzOptionsBuilt = true;
}
async function setTimezone(){
  const sel = document.getElementById('tzSelect');
  let tz = sel.value;
  if (tz === '__custom__') {
    tz = prompt('Enter a POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3');
    if (!tz) { tzUserIsEditing = false; return; }
  }
  await fetch(`/api/timezone?tz=${encodeURIComponent(tz)}`, {method:'POST'});
  tzUserIsEditing = false;
}
async function loadStatus(){
  const r = await fetch('/api/status'); const s = await r.json();
  const gps = s.gpsFix ? `fix (${s.gpsSatellites} sats)` : `no fix (${s.gpsSatellites} sats)`;
  document.getElementById('status').innerText =
    `Battery: ${s.battery}%${s.charging ? (s.battery >= 100 ? ' (fully charged)' : ' (charging)') : ''} | Wifi: ${s.wifi} | GPS: ${gps} | FW: ${s.fw} | RAM: ${s.ramPointsUsed}/${s.ramPointsCapacity} pts (${s.ramPointsFree} free)`;
  document.getElementById('statusTime').innerText = s.timeSynced
    ? `Date: ${s.boardDateLocal} | Time: ${s.boardTimeLocal}${s.isDst ? ' (DST)' : ''}`
    : 'Board time: not synced yet (waiting for first GPS fix)';

  window._backendHost = s.backendHost; window._backendPort = s.backendPort;

  const sy = s.sync;
  let syncText;
  if (sy.syncing) {
    syncText = `Sync: uploading ${sy.current}/${sy.total}...`;
  } else if (sy.uploadsGaveUp) {
    syncText = `Sync: backend unreachable (gave up after ${sy.uploadFails} failures) - ${sy.pending} pending, retries next wake`;
  } else if (sy.pending === 0) {
    syncText = 'Sync: all sessions uploaded';
  } else {
    syncText = `Sync: ${sy.pending} session(s) pending`
      + (sy.uploadFails ? ` (${sy.uploadFails} failed attempt(s))` : '');
  }
  if (sy.lastAttemptAgoS !== null && !sy.syncing) {
    syncText += ` | last attempt ${sy.lastAttemptAgoS}s ago (${sy.lastUploaded} uploaded)`;
  }
  syncText += ` | Backend: ${s.backendHost}:${s.backendPort}`;
  document.getElementById('statusSync').innerText = syncText;
  // Poll faster while a pass is running so the counter visibly advances
  if (sy.syncing && !window._syncFastPoll) {
    window._syncFastPoll = setInterval(loadStatus, 2000);
  } else if (!sy.syncing && window._syncFastPoll) {
    clearInterval(window._syncFastPoll); window._syncFastPoll = null;
  }

  if (!tzOptionsBuilt) buildTimezoneOptions();
  if (!tzUserIsEditing) {
    const sel = document.getElementById('tzSelect');
    const match = TZ_PRESETS.some(([tz]) => tz === s.tzPosix);
    sel.value = match ? s.tzPosix : '__custom__';
  }
}
async function loadSatellites(){
  const r = await fetch('/api/satellites'); const data = await r.json();
  document.getElementById('satSummary').innerText =
    `${data.count} in view, ${data.usedInFix} used in fix`;
  const tbody = document.getElementById('satBody'); tbody.innerHTML = '';
  if (data.satellites.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" style="color:#888">No satellites in view yet</td></tr>';
    return;
  }
  // Strongest signal first - the ones actually worth looking at belong
  // at the top; untracked (snr === null) sort to the bottom.
  data.satellites.slice().sort((a, b) => (b.snr ?? -1) - (a.snr ?? -1)).forEach(s => {
    const pct = s.tracked ? Math.max(0, Math.min(100, Math.round((s.snr / 50) * 100))) : 0;
    const color = pct > 60 ? '#4ea36f' : pct > 30 ? '#e0c05a' : '#e05a5a';
    const tr = document.createElement('tr');
    tr.innerHTML =
      `<td>${s.name} ${s.prn}</td><td>${s.elevation}\u00b0</td><td>${s.azimuth}\u00b0</td>` +
      `<td>${s.tracked ? s.snr + ' dB-Hz' : '\u2014'}</td>` +
      `<td><div class="sat-bar"><div class="sat-bar-fill" style="width:${pct}%;background:${color}"></div></div></td>`;
    tbody.appendChild(tr);
  });
}
// Only polls while this tab is actually visible ("when UI is active") -
// no point waking the device's WebServer to serve this every few
// seconds if nobody's looking at the page.
let satInterval = null;
function startSatPolling(){
  loadSatellites();
  if (!satInterval) satInterval = setInterval(loadSatellites, 3000);
}
function stopSatPolling(){
  if (satInterval) { clearInterval(satInterval); satInterval = null; }
}
document.addEventListener('visibilitychange', () => {
  if (document.hidden) stopSatPolling(); else startSatPolling();
});
async function loadSessions(){
  const r = await fetch('/api/sessions'); const data = await r.json();
  const el = document.getElementById('calendar'); el.innerHTML='';
  if (Object.keys(data).length === 0) { el.innerHTML = '<span class="status">No tracks recorded yet.</span>'; return; }
  Object.keys(data).forEach(date=>{
    const card=document.createElement('div'); card.className='date-block';
    const head=document.createElement('div'); head.className='date';
    head.innerHTML = `${date} <a class="btn" href="#" onclick="deleteDate('${date}'); return false;" style="font-size:.8rem;font-weight:400;margin-left:.6rem">delete date</a>`;
    card.appendChild(head);
    data[date].forEach(f=>{
      const row=document.createElement('div'); row.className='session';
      row.innerHTML = `<span>${f}</span><span>
        <a class="btn" href="/api/download?date=${date}&file=${f}">download</a>
        <a class="btn" href="#" onclick="deleteSession('${date}','${f}'); return false;" style="margin-left:.6rem">delete</a>
      </span>`;
      card.appendChild(row);
    });
    el.appendChild(card);
  });
}
async function deleteSession(date, file){
  if (!confirm(`Delete ${file} (${date})? This can't be undone.`)) return;
  await fetch(`/api/sessions?date=${date}&file=${file}`, {method:'DELETE'});
  loadSessions();
}
async function deleteDate(date){
  if (!confirm(`Delete ALL sessions for ${date}? This can't be undone.`)) return;
  await fetch(`/api/sessions?date=${date}`, {method:'DELETE'});
  loadSessions();
}
async function syncNow(){ await fetch('/api/sync',{method:'POST'}); alert('Sync requested'); }

async function changeBackend(){
  const host = prompt('Backend host or IP', window._backendHost || '');
  if (!host) return;
  const port = prompt('Backend REST port', window._backendPort || 8080);
  if (!port) return;
  const url = `/api/backend?host=${encodeURIComponent(host)}&port=${encodeURIComponent(port)}`;
  const r = await fetch(url, {method:'POST'});
  if (!r.ok) { alert('Failed: ' + await r.text()); return; }
  alert('Backend updated - a sync attempt starts immediately.');
  loadStatus();
}

async function resetWifi(){
  if (!confirm('This erases the saved WiFi credentials and reboots the device into setup mode. Backend settings are kept. Continue?')) return;
  const link = document.getElementById('resetWifiLink');
  link.innerText = 'Resetting...';
  try {
    await fetch('/api/reset-wifi', {method:'POST'});
  } catch (e) { /* device is rebooting, connection may drop mid-response */ }
  alert('WiFi reset - the device is rebooting into setup mode. Connect to its GPSLogger-Setup-XXXX WiFi network to reconfigure it.');
}

function flashFirmware(){
  const fileInput = document.getElementById('otaFile');
  const btn = document.getElementById('otaBtn');
  const bar = document.getElementById('otaBar');
  const label = document.getElementById('otaLabel');
  if (!fileInput.files.length) { alert('Choose a firmware .bin file first'); return; }

  btn.disabled = true;
  bar.classList.remove('error');
  bar.style.width = '0%';
  label.innerText = 'Uploading...';

  const xhr = new XMLHttpRequest();
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      bar.style.width = pct + '%';
      label.innerText = `Uploading & flashing: ${pct}%`;
    }
  };
  xhr.onload = async () => {
    if (xhr.status === 200) {
      bar.style.width = '100%';
      label.innerText = 'Verifying flash...';
      // Confirm the device-side write actually succeeded before promising a reboot.
      try {
        const r = await fetch('/api/ota/status');
        const s = await r.json();
        if (s.error) {
          bar.classList.add('error');
          label.innerText = 'Flash failed: ' + s.error;
          btn.disabled = false;
          return;
        }
      } catch (e) { /* device may already be rebooting; fall through */ }
      label.innerText = 'Flash complete - device rebooting...';
      let secs = 8;
      const iv = setInterval(() => {
        secs--;
        label.innerText = `Rebooting... reconnecting in ${secs}s`;
        if (secs <= 0) { clearInterval(iv); location.reload(); }
      }, 1000);
    } else {
      bar.classList.add('error');
      label.innerText = 'Flash failed: ' + xhr.responseText;
      btn.disabled = false;
    }
  };
  xhr.onerror = () => {
    bar.classList.add('error');
    label.innerText = 'Upload error - check connection and try again.';
    btn.disabled = false;
  };
  const formData = new FormData();
  formData.append('firmware', fileInput.files[0]);
  xhr.open('POST', '/update');
  xhr.send(formData);
}

loadStatus(); loadSessions(); setInterval(loadStatus, 10000);
if (!document.hidden) startSatPolling();
</script></body></html>
)HTML";

void WebUi::begin() {
    _server.on("/", HTTP_GET, [this]() { handleRoot(); });
    _server.on("/api/sessions", HTTP_GET, [this]() { handleApiSessions(); });
    _server.on("/api/download", HTTP_GET, [this]() { handleApiDownload(); });
    _server.on("/api/status", HTTP_GET, [this]() { handleApiStatus(); });
    _server.on("/api/satellites", HTTP_GET, [this]() { handleApiSatellites(); });
    _server.on("/api/sync", HTTP_POST, [this]() { handleApiSyncNow(); });
    _server.on("/api/sessions", HTTP_DELETE, [this]() { handleApiDelete(); });
    _server.on("/api/reset-wifi", HTTP_POST, [this]() { handleApiResetWifi(); });
    _server.on("/api/timezone", HTTP_POST, [this]() { handleApiSetTimezone(); });
    _server.on("/api/backend", HTTP_POST, [this]() { handleApiSetBackend(); });

    _server.on("/api/ota/status", HTTP_GET, [this]() {
        JsonDocument doc;
        doc["active"] = otaActive;
        doc["written"] = otaWrittenBytes;
        doc["success"] = otaLastSuccess;
        if (otaLastError.length()) doc["error"] = otaLastError;
        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    });

    // OTA firmware upload: POST /update with multipart file "firmware".
    // Progress is tracked client-side (browser knows the file size via the
    // upload.onprogress event), since flash writes happen synchronously as
    // each chunk streams in; /api/ota/status lets the UI confirm success
    // before promising a reboot.
    _server.on("/update", HTTP_POST,
        [this]() {
            bool ok = !Update.hasError();
            otaLastSuccess = ok;
            _server.sendHeader("Connection", "close");
            _server.send(200, "text/plain", ok ? "OK, rebooting" : otaLastError.c_str());
            if (ok) {
                delay(500);
                ESP.restart();
            }
        },
        [this]() { handleOtaUpload(); }
    );

    _server.begin();
}

void WebUi::loop() {
    _server.handleClient();
}

void WebUi::handleRoot() {
    _server.send_P(200, "text/html", INDEX_HTML);
}

void WebUi::handleApiSessions() {
    JsonDocument doc;
    auto dates = trackStore.listSessionDates();
    for (auto &d : dates) {
        JsonArray arr = doc[d].to<JsonArray>();
        for (auto &s : trackStore.listSessionsForDate(d)) arr.add(s);
    }
    String out;
    serializeJson(doc, out);
    _server.send(200, "application/json", out);
}

void WebUi::handleApiDownload() {
    if (!_server.hasArg("date") || !_server.hasArg("file")) {
        _server.send(400, "text/plain", "missing date/file");
        return;
    }
    String path = trackStore.sessionPath(_server.arg("date"), _server.arg("file"));
    if (!LittleFS.exists(path)) {
        _server.send(404, "text/plain", "not found");
        return;
    }
    File f = LittleFS.open(path, "r");
    // Explicit filename, rather than relying on the browser to infer one
    // from the URL's query string - ensures what's saved locally always
    // matches the file's own name (and its GPX <name> tag, since both are
    // derived from the same session identifier - see TrackStore).
    _server.sendHeader("Content-Disposition", "attachment; filename=\"" + _server.arg("file") + "\"");
    _server.streamFile(f, "application/gpx+xml");
    f.close();
}

// Rough PRN -> constellation mapping (varies a bit by receiver/NMEA
// revision, but close enough for display purposes - not used for
// anything functional). This module only reports GPS, but
// parseGsvLine() in gps_reader.cpp deliberately doesn't filter by
// talker ID, so this covers other constellations too in case a
// different module ever ends up here.
static const char *constellationName(uint8_t prn) {
    if (prn >= 1 && prn <= 32) return "GPS";
    if (prn >= 33 && prn <= 64) return "SBAS";
    if (prn >= 65 && prn <= 96) return "GLONASS";
    if (prn >= 201 && prn <= 263) return "BeiDou";
    if (prn >= 301 && prn <= 336) return "Galileo";
    return "Unknown";
}

void WebUi::handleApiSatellites() {
    JsonDocument doc;
    JsonArray arr = doc["satellites"].to<JsonArray>();
    size_t count = gpsReader.satelliteListCount();
    const SatelliteInfo *sats = gpsReader.satelliteList();
    for (size_t i = 0; i < count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["prn"] = sats[i].prn;
        o["name"] = constellationName(sats[i].prn);
        o["elevation"] = sats[i].elevation;
        o["azimuth"] = sats[i].azimuth;
        // snr < 0 means "in view but not currently tracked" (GSV left the
        // field blank) - sent as null rather than a fabricated number, and
        // "tracked" spelled out separately so the UI doesn't have to
        // infer it from a magic sentinel value.
        if (sats[i].snr >= 0) o["snr"] = sats[i].snr;
        else o["snr"] = nullptr;
        o["tracked"] = sats[i].snr >= 0;
    }
    doc["count"] = count;
    doc["usedInFix"] = gpsReader.satellites(); // for comparison against "in view"

    String out;
    serializeJson(doc, out);
    _server.send(200, "application/json", out);
}

void WebUi::handleApiStatus() {
    JsonDocument doc;
    doc["battery"] = powerManager.readBatteryPercent();
    doc["charging"] = powerManager.isCharging();
    doc["wifi"] = syncManager.isWifiConnected() ? "connected" : "offline";
    doc["fw"] = FW_VERSION;
    doc["ramPointsUsed"] = trackStore.ramPointCount();
    doc["ramPointsCapacity"] = RAM_TRACKPOINT_CAPACITY;
    doc["ramPointsFree"] = RAM_TRACKPOINT_CAPACITY - trackStore.ramPointCount();
    doc["deviceId"] = configManager.deviceId();
    doc["backendHost"] = configManager.get().backendHost;
    doc["backendPort"] = configManager.get().backendPort;

    doc["gpsFix"] = gpsReader.hasFix();
    doc["gpsSatellites"] = gpsReader.satellites();

    // Sync machinery state - pending files, live progress of a running
    // upload pass (reachable mid-pass because syncPendingSessions services
    // this server between files), and the per-wake-cycle give-up flags.
    SyncStatus ss = syncManager.getStatus();
    JsonObject sync = doc["sync"].to<JsonObject>();
    sync["pending"] = ss.pendingSessions;
    sync["syncing"] = ss.syncing;
    sync["current"] = ss.syncCurrent;
    sync["total"] = ss.syncTotal;
    sync["lastUploaded"] = ss.lastUploaded;
    sync["uploadFails"] = ss.uploadFails;
    sync["uploadsGaveUp"] = ss.uploadsGaveUp;
    if (ss.lastAttemptMs > 0) {
        sync["lastAttemptAgoS"] = (uint32_t)((millis() - ss.lastAttemptMs) / 1000UL);
    } else {
        sync["lastAttemptAgoS"] = nullptr;
    }

    // The clock is only meaningful once GPS has delivered a fix at least
    // once (see gps_reader.cpp - it's synced from GPS NMEA data, not an
    // RTC chip); before that it reads epoch-0-ish, which would just show
    // as a confusing 1970 date, so flag it explicitly instead.
    time_t now = time(nullptr);
    bool timeSynced = now >= PLAUSIBLE_EPOCH_FLOOR;
    doc["timeSynced"] = timeSynced;
    doc["tzPosix"] = configManager.get().tzPosix;
    if (timeSynced) {
        struct tm tmUtc;
        gmtime_r(&now, &tmUtc);
        char iso[25];
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
        doc["boardTime"] = iso; // UTC, ISO 8601 - for API consumers

        // Local display time: localtime_r() applies whatever TZ string was
        // set via ConfigManager::applyTimezone() (setenv+tzset at boot and
        // on change), including DST, automatically - no manual offset math.
        struct tm tmLocal;
        localtime_r(&now, &tmLocal);
        char dateBuf[11], timeBuf[9];
        strftime(dateBuf, sizeof(dateBuf), "%d/%m/%Y", &tmLocal);
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmLocal);
        doc["boardDateLocal"] = dateBuf;
        doc["boardTimeLocal"] = timeBuf;
        doc["isDst"] = tmLocal.tm_isdst > 0;
    } else {
        doc["boardTime"] = nullptr;
        doc["boardDateLocal"] = nullptr;
        doc["boardTimeLocal"] = nullptr;
        doc["isDst"] = nullptr;
    }

    String out;
    serializeJson(doc, out);
    _server.send(200, "application/json", out);
}

void WebUi::handleApiSyncNow() {
    syncManager.requestImmediateSync();
    _server.send(202, "text/plain", "sync requested");
}

// Handlers that mutate session files or sync/device state must not run
// while a sync pass is mid-flight: the web server is deliberately serviced
// between file uploads (live progress), so these handlers are reentrant
// into SyncManager's iteration - deleting a file it is about to upload
// would count as a spurious upload failure (10 of those disable uploads for
// the whole wake cycle), and an OTA flash would run concurrently with an
// active HTTP upload. Refusing with 503 keeps the reentrancy read-only.
bool WebUi::rejectIfSyncing() {
    if (syncManager.isSyncing()) {
        _server.send(503, "text/plain", "sync in progress - retry in a few seconds");
        return true;
    }
    return false;
}

void WebUi::handleApiDelete() {
    if (rejectIfSyncing()) return;
    if (!_server.hasArg("date")) {
        _server.send(400, "text/plain", "missing date");
        return;
    }
    String date = _server.arg("date");
    bool ok;
    if (_server.hasArg("file")) {
        ok = trackStore.deleteSession(date, _server.arg("file"));
    } else {
        ok = trackStore.deleteDate(date);
    }
    syncManager.invalidatePendingCache(); // deleted files change the pending count
    _server.send(ok ? 200 : 404, "text/plain", ok ? "deleted" : "not found or delete failed");
}

void WebUi::handleApiResetWifi() {
    if (rejectIfSyncing()) return;
    // Erase only the WiFi SSID/password from the ESP-IDF's own storage -
    // backend host/port/MQTT settings in config.json are untouched, so the
    // captive portal will still prefill them on next setup. On reboot,
    // ConfigManager::isWifiConfigured() sees no saved SSID and main.cpp
    // automatically opens the setup portal (see main.cpp - there's no
    // physical button for this anymore, this link is the only trigger).
    Serial.println("[WiFi] Reset requested via web UI - erasing credentials and rebooting");
    _server.send(200, "text/plain", "WiFi credentials cleared - rebooting into setup mode");
    _server.client().flush();
    delay(300);
    WiFi.disconnect(true, true);
    delay(200);
    ESP.restart();
}

void WebUi::handleApiSetTimezone() {
    if (!_server.hasArg("tz")) {
        _server.send(400, "text/plain", "missing tz");
        return;
    }
    String tz = _server.arg("tz");
    if (!configManager.setTimezone(tz)) {
        _server.send(400, "text/plain", "invalid or too-long tz string");
        return;
    }
    Serial.printf("[Config] Timezone set to %s\n", tz.c_str());
    _server.send(200, "text/plain", "ok");
}

void WebUi::handleApiSetBackend() {
    if (rejectIfSyncing()) return;
    if (!_server.hasArg("host")) {
        _server.send(400, "text/plain", "missing host");
        return;
    }
    String host = _server.arg("host");
    host.trim();
    auto &cfg = configManager.get();
    if (host.length() == 0 || host.length() >= sizeof(cfg.backendHost)) {
        _server.send(400, "text/plain", "invalid or too-long host");
        return;
    }
    uint16_t port = cfg.backendPort;
    if (_server.hasArg("port")) {
        long p = _server.arg("port").toInt();
        if (p < 1 || p > 65535) {
            _server.send(400, "text/plain", "invalid port");
            return;
        }
        port = (uint16_t)p;
    }
    strlcpy(cfg.backendHost, host.c_str(), sizeof(cfg.backendHost));
    cfg.backendPort = port;
    configManager.save();
    // The old backend being unreachable says nothing about the new one -
    // clear any give-up state and try immediately, no reboot needed. Also
    // forget which sessions were already uploaded: the sync ledger doesn't
    // know WHICH backend it synced to, so keeping it after a backend change
    // would silently skip every older session on the new server. Setting
    // the backend (even to the same address) thus doubles as a
    // user-accessible "re-sync everything" action.
    syncManager.resetBackoff();
    syncManager.clearSyncState();
    syncManager.requestImmediateSync();
    Serial.printf("[Config] Backend set to %s:%u via web UI\n", cfg.backendHost, cfg.backendPort);
    _server.send(200, "text/plain", "ok");
}

void WebUi::handleOtaUpload() {
    HTTPUpload &upload = _server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        if (syncManager.isSyncing()) {
            // Can't cleanly send a 503 from inside a streaming upload
            // handler; mark the error so the completion handler and
            // /api/ota/status report it, and never call Update.begin().
            otaLastError = "sync in progress - retry OTA in a few seconds";
            otaLastSuccess = false;
            return;
        }
        Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
        otaActive = true;
        otaWrittenBytes = 0;
        otaLastError = "";
        otaLastSuccess = false;
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            otaLastError = "Update.begin() failed - check partition scheme has room for OTA";
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!Update.isRunning()) return; // begin() refused/never ran - drain the stream silently
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            otaLastError = "Flash write failed mid-upload";
            Update.printError(Serial);
        } else {
            otaWrittenBytes += upload.currentSize;
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        otaActive = false;
        if (!Update.isRunning()) return; // begin() refused/never ran - error already recorded
        if (Update.end(true)) {
            Serial.printf("[OTA] Success, %u bytes\n", upload.totalSize);
        } else {
            otaLastError = "Update.end() failed - image may be corrupt or too large";
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        otaActive = false;
        otaLastError = "Upload aborted";
        Update.end(false);
    }
}
