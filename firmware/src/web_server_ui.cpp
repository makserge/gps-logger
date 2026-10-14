#include "web_server_ui.h"
#include "debug_log.h"
#include "track_store.h"
#include "power_manager.h"
#include "sync_manager.h"
#include "config_manager.h"
#include "gps_reader.h"
#include "wifi_connector.h"
#include "lifecycle.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_wifi.h>
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
/* Dark palette (night). The light palette below is switched in by
   applyTheme() from the browser's local time of day. */
:root{color-scheme:dark;
  --bg:#111;--fg:#eee;--card:#1c1c1c;--line:#333;--accent:#4ea3ff;--ok:#9a9;--muted:#888;
  --ctl-bg:#2a2a2a;--ctl-border:#444;--track:#333;
  --cal-dim:#666;--cal-in:#ddd;--cal-has-bg:#1d3350;--cal-has-fg:#9cc8ff;--cal-has-border:#2c4f7a;--cal-sel-bg:#24416a;
  --danger:#e05a5a;--danger-border:#6a3030;--alert-bg:#4a1515;--alert-fg:#ffd9d9;--alert-strong:#fff;--log-bg:#0b0b0b}
:root[data-theme=light]{color-scheme:light;
  --bg:#f1f2f4;--fg:#1c1c1c;--card:#fff;--line:#dcdcdc;--accent:#1f6fd1;--ok:#3d7a3d;--muted:#777;
  --ctl-bg:#f7f7f7;--ctl-border:#bbb;--track:#e3e3e3;
  --cal-dim:#b0b0b0;--cal-in:#222;--cal-has-bg:#dbe9fb;--cal-has-fg:#1c4f8f;--cal-has-border:#a9c7ec;--cal-sel-bg:#bcd6f5;
  --danger:#c43c3c;--danger-border:#e3a3a3;--alert-bg:#fde3e3;--alert-fg:#6b1a1a;--alert-strong:#3a0d0d;--log-bg:#fafafa}
body{font-family:system-ui,sans-serif;margin:0;padding:1rem;background:var(--bg);color:var(--fg)}
h1{font-size:1.2rem} .card{background:var(--card);border-radius:8px;padding:1rem;margin-bottom:.75rem}
.date{font-weight:600;margin-bottom:.4rem;cursor:pointer}
.date-block{margin-bottom:1rem} .date-block:last-child{margin-bottom:0}
.section-title{font-weight:600;margin-bottom:.6rem}
.session{display:flex;justify-content:space-between;padding:.4rem 0;border-top:1px solid var(--line)}
a.btn{color:var(--accent);text-decoration:none} .status{font-size:.85rem;color:var(--ok)}
.muted{color:var(--muted)}
button{background:var(--ctl-bg);color:var(--fg);border:1px solid var(--ctl-border);border-radius:6px;padding:.5rem .8rem}
button:disabled{opacity:.5}
input[type=file]{color:var(--fg);font-size:.85rem}
.progress-track{background:var(--ctl-bg);border-radius:4px;height:10px;overflow:hidden;margin-top:.6rem}
.progress-bar{background:var(--accent);height:100%;width:0%;transition:width .2s}
.progress-bar.error{background:var(--danger)}
.sat-table{width:100%;border-collapse:collapse;font-size:.8rem}
.sat-table th,.sat-table td{padding:.3rem .4rem;text-align:left;border-top:1px solid var(--line)}
.sat-bar{background:var(--track);border-radius:3px;overflow:hidden;width:60px;height:8px;display:inline-block}
.sat-bar-fill{height:100%}
.cal-head,.cal-grid{max-width:630px;margin-left:auto;margin-right:auto}
.cal-head{display:flex;justify-content:space-between;align-items:center;margin-bottom:.6rem}
.cal-title{font-weight:600}
.cal-grid{display:grid;grid-template-columns:repeat(7,1fr);gap:4px}
.cal-dow{text-align:center;font-size:.7rem;color:var(--muted);padding:.2rem 0}
.cal-day{aspect-ratio:1.4;display:flex;flex-direction:column;align-items:center;
  justify-content:center;border-radius:8px;font-size:.85rem;color:var(--cal-dim);border:1px solid transparent}
.cal-day.in-month{color:var(--cal-in)}
.cal-day.has-tracks{background:var(--cal-has-bg);color:var(--cal-has-fg);cursor:pointer;border-color:var(--cal-has-border)}
.cal-day.has-tracks:hover{border-color:var(--accent)}
.cal-day.selected{border-color:var(--accent);background:var(--cal-sel-bg)}
.cal-day.today{outline:1px dashed var(--cal-dim)}
.cal-count{font-size:.6rem;color:var(--accent);line-height:1;margin-top:2px}
#daySessions{margin-top:.9rem}
.session:first-of-type{border-top:none}
a.btn-danger{color:var(--danger)}
.topbar{display:flex;justify-content:space-between;align-items:baseline}
.alert{background:var(--alert-bg);border:1px solid var(--danger);color:var(--alert-fg);border-radius:8px;padding:.8rem 1rem;
  margin-bottom:.75rem;font-size:.9rem;line-height:1.4}
.alert b{color:var(--alert-strong)}
.logview{background:var(--log-bg);border:1px solid var(--line);border-radius:6px;padding:.6rem;margin-top:.6rem;
  max-height:420px;overflow:auto;font-size:.72rem;line-height:1.35;white-space:pre-wrap;word-break:break-word}
button.btn-danger{color:var(--danger);border-color:var(--danger-border)}
select{background:var(--ctl-bg);color:var(--fg);border:1px solid var(--ctl-border);border-radius:6px;padding:.4rem .5rem;max-width:100%}
.bf-row{display:flex;align-items:center;gap:.4rem;margin-bottom:.5rem;flex-wrap:wrap}
.bf-row label{width:9rem;font-size:.85rem}
.bf-row input{background:var(--ctl-bg);color:var(--fg);border:1px solid var(--ctl-border);border-radius:6px;padding:.4rem .5rem}
.bf-row input[type=number]{width:5.5rem}
</style>
<script>
// Light theme by day, dark by night - decided from the viewing browser's
// local clock, re-checked every minute. Runs before the body renders so
// the page never flashes the wrong theme on load.
function applyTheme(){
  const h = new Date().getHours();
  const light = h >= 7 && h < 19;
  document.documentElement.setAttribute('data-theme', light ? 'light' : 'dark');
}
applyTheme(); setInterval(applyTheme, 60000);
</script>
</head><body>
<div id="mainView">
<div class="topbar"><h1>GPS Logger</h1><a class="btn" href="#settings">&#9881; Settings</a></div>
<div class="alert" id="storageWarn" style="display:none"></div>
<div class="card">
  <div class="status" id="status">loading status...</div>
  <div class="status" id="statusTime" style="margin-top:.3rem"></div>
  <div class="status" id="statusSync" style="margin-top:.3rem"></div>
  <div style="margin-top:.6rem">
    <button onclick="syncNow()">Sync now</button>
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
  <div class="section-title">Tracks</div>
  <div id="calWrap" style="display:none">
    <div class="cal-head">
      <button onclick="shiftMonth(-1)">&#9664;</button>
      <span class="cal-title" id="calTitle"></span>
      <button onclick="shiftMonth(1)">&#9654;</button>
    </div>
    <div class="cal-grid" id="calGrid"></div>
  </div>
  <div id="daySessions"><span class="status">loading sessions...</span></div>
</div>
</div>
<div id="settingsView" style="display:none">
<div class="topbar"><h1>Settings</h1><a class="btn" href="#">&#8592; Back</a></div>
<div class="card">
  <div class="section-title">Power</div>
  <div class="status">Switches the device off completely: no motion detection, no tracking, no WiFi. It switches itself back on as soon as a charger is connected (USB). Use it when the logger is going to sit unused for a while.</div>
  <div style="margin-top:.6rem"><button id="shutdownBtn" class="btn-danger" onclick="shutdownDevice()">Shut down</button></div>
</div>
<div class="card">
  <div class="section-title">Backend</div>
  <div class="bf-row"><label for="bfHost">Primary backend</label>
    <input id="bfHost" placeholder="host or IP" disabled><input id="bfPort" type="number" min="1" max="65535" placeholder="port" disabled></div>
  <div class="bf-row"><label for="bfHost2">Second backend</label>
    <input id="bfHost2" placeholder="optional - empty = none" disabled><input id="bfPort2" type="number" min="1" max="65535" placeholder="port" disabled></div>
  <div class="status">The device tries the primary first, then the second, and syncs to whichever answers.</div>
  <div class="status" id="bfActive"></div>
  <div style="margin-top:.6rem">
    <button id="bfSave" onclick="saveBackend()" disabled>Save</button>
    <button onclick="revertBackendForm()" style="margin-left:.4rem">Revert</button>
    <span class="status" id="bfMsg" style="margin-left:.6rem"></span>
  </div>
</div>
<div class="card">
  <div class="section-title">Timezone</div>
  <select id="tzSelect" onchange="setTimezone()"></select>
  <a class="btn" href="#settings" id="tzEdit" onclick="editCustomTz(); return false;" style="margin-left:.6rem;display:none">edit</a>
  <div class="status" id="tzCurrent"></div>
  <div class="status">Used for the board clock and for grouping tracks by local date.</div>
</div>
<div class="card">
  <div class="section-title">WiFi</div>
  <div class="bf-row"><label for="wfSsid">Primary network</label>
    <input id="wfSsid" placeholder="network name (SSID)" maxlength="32" disabled>
    <input id="wfPass" type="password" autocomplete="new-password" maxlength="64" disabled></div>
  <div class="bf-row"><label for="wfSsid2">Second network</label>
    <input id="wfSsid2" placeholder="optional - empty = none" maxlength="32" disabled>
    <input id="wfPass2" type="password" autocomplete="new-password" maxlength="64" disabled></div>
  <div class="status">The device tries the network that worked last, then the other one. Leave a password empty to keep the saved one.</div>
  <div class="status" id="wifiInfo"></div>
  <div style="margin-top:.6rem">
    <button id="wfSave" onclick="saveWifi()" disabled>Save</button>
    <button onclick="revertWifiForm()" style="margin-left:.4rem">Revert</button>
    <span class="status" id="wfMsg" style="margin-left:.6rem"></span>
  </div>
  <div style="margin-top:1rem;padding-top:.8rem;border-top:1px solid var(--line)">
    <button id="resetWifiBtn" class="btn-danger" onclick="resetWifi()">Reset WiFi</button>
    <div class="status">Erases both saved networks and reboots into setup mode. Backend settings are kept.</div>
  </div>
</div>
<div class="card">
  <div class="section-title">Storage</div>
  <div class="status" id="stInfo"></div>
  <label style="font-size:.9rem"><input type="checkbox" id="stReclaim" onchange="setReclaim()"> Delete uploaded tracks when space runs out</label>
  <div class="status">Only as a last resort: the device keeps every track it has room for, and deletes its oldest tracks that are already on a backend only when new points would not fit otherwise. Switched off, nothing is ever deleted automatically - when the flash is full, recording stops until you delete tracks.</div>
</div>
<div class="card">
  <div class="section-title">Debug log</div>
  <label style="font-size:.9rem"><input type="checkbox" id="dbgOn" onchange="setDebug()"> Record debug log</label>
  <div class="status">Records detailed, timestamped events (WiFi, backends, sync, sleep, reboots and their reason) to the device's flash, kept across reboots and sleep (max ~100 KB). Switch it off when not diagnosing a problem - it adds flash writes.</div>
  <div class="status" id="dbgInfo"></div>
  <div style="margin-top:.6rem">
    <button id="dbgShow" onclick="showLog()">Show log</button>
    <a class="btn" href="/api/log?download=1" style="margin-left:.6rem">download</a>
    <a class="btn btn-danger" href="#" onclick="clearLog(); return false;" style="margin-left:.6rem">clear</a>
  </div>
  <pre id="dbgView" class="logview" style="display:none"></pre>
</div>
<div class="card">
  <div class="section-title">Firmware update (OTA)</div>
  <div class="status" id="fwCurrent" style="margin-bottom:.6rem"></div>
  <input type="file" id="otaFile" accept=".bin">
  <button id="otaBtn" onclick="flashFirmware()">Flash</button>
  <div class="progress-track"><div class="progress-bar" id="otaBar"></div></div>
  <div class="status" id="otaLabel">Select a firmware.bin file to flash over WiFi.</div>
</div>
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
async function setTimezone(customTz){
  const sel = document.getElementById('tzSelect');
  let tz = customTz || sel.value;
  if (tz === '__custom__') {
    tz = prompt('Enter a POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3', window._tzPosix || '');
    if (!tz) { tzUserIsEditing = false; loadStatus(); return; }
  }
  const r = await fetch(`/api/timezone?tz=${encodeURIComponent(tz)}`, {method:'POST'});
  tzUserIsEditing = false;
  if (!r.ok) alert('Timezone not saved: ' + await r.text());
  loadStatus(); // re-sync the select with what the device actually has
}
// Selecting "Custom..." again while a custom zone is active fires no
// change event - this link edits it directly, prefilled.
function editCustomTz(){
  const tz = prompt('Enter a POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3', window._tzPosix || '');
  if (tz) setTimezone(tz);
}
async function loadStatus(){
  const r = await fetch('/api/status'); const s = await r.json();
  window._tzPosix = s.tzPosix;
  const gps = s.gpsFix ? `fix (${s.gpsSatellites} sats)` : `no fix (${s.gpsSatellites} sats)`;
  document.getElementById('status').innerText =
    `Battery: ${s.battery}%${s.charging ? (s.battery >= 100 ? ' (fully charged)' : ' (charging)') : ''} | Wifi: ${s.wifi}${s.wifiConnectedSsid ? ` (${s.wifiConnectedSsid})` : ''} | GPS: ${gps} | FW: ${s.fw} | RAM: ${s.ramPointsUsed}/${s.ramPointsCapacity} pts (${s.ramPointsFree} free) | Flash: ${s.fsUsedKB}/${s.fsTotalKB} KB`;
  document.getElementById('statusTime').innerText = s.timeSynced
    ? `Date: ${s.boardDateLocal} | Time: ${s.boardTimeLocal}${s.isDst ? ' (DST)' : ''}`
    : 'Board time: not synced yet (waiting for first GPS fix)';

  window._backendHost = s.backendHost; window._backendPort = s.backendPort;
  window._backendHost2 = s.backendHost2; window._backendPort2 = s.backendPort2;
  if (!backendFormDirty) fillBackendForm();
  document.getElementById('bfActive').innerText = s.activeBackend
    ? `Currently syncing to: ${s.activeBackend}`
    : 'Currently syncing to: none reachable yet';
  window._wifi = { ssid: s.wifiSsid || '', hasPass: !!s.wifiHasPass, ssid2: s.wifiSsid2 || '', hasPass2: !!s.wifiHasPass2 };
  if (!wifiFormDirty) fillWifiForm();
  document.getElementById('stReclaim').checked = !!s.reclaimSynced;
  const st = s.storage || {};
  document.getElementById('stInfo').innerText = `Flash: ${s.fsUsedKB} of ${s.fsTotalKB} KB used - `
    + `${st.unsyncedKB ?? '?'} KB in ${st.unsyncedFiles ?? '?'} track(s) not uploaded yet`
    + (s.reclaimSynced ? `, ${st.reclaimableKB ?? '?'} KB of uploaded tracks can be removed if needed` : '');
  // Red warning: only not-uploaded tracks (never deleted) are left to fill
  // the flash. textContent-only rendering (values are numbers anyway).
  const warn = document.getElementById('storageWarn');
  if (st.level >= 1) {
    const left = (st.freeKB || 0) + (st.reclaimableKB || 0);
    warn.textContent = '';
    const head = document.createElement('b');
    head.textContent = st.level >= 2 ? 'Storage full - recording will stop. ' : 'Storage almost full. ';
    warn.appendChild(head);
    warn.appendChild(document.createTextNode(
      `Only ${left} KB left; ${st.unsyncedKB} KB in ${st.unsyncedFiles} track(s) are not uploaded yet `
      + `and are never deleted automatically. Bring the device into WiFi range of a backend so they upload `
      + `(space is then freed automatically), or download and delete tracks below.`));
    warn.style.display = '';
  } else {
    warn.style.display = 'none';
  }
  document.getElementById('dbgOn').checked = !!s.debugLog;
  document.getElementById('dbgInfo').innerText = s.debugLog
    ? `Recording - ${s.debugLogKB} KB stored`
    : (s.debugLogKB ? `Off - ${s.debugLogKB} KB from earlier recording stored` : 'Off');
  document.getElementById('wifiInfo').innerText = s.wifiConnectedSsid
    ? `Connected to "${s.wifiConnectedSsid}" (${s.wifiRssi} dBm)`
    : 'Not connected';
  document.getElementById('fwCurrent').innerText = `Current firmware: ${s.fw} | Device ID: ${s.deviceId}`;

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
  if (sy.rejected) syncText += ` | ${sy.rejected} skipped (refused or failing - Sync now retries)`;
  if (sy.lastAttemptAgoS !== null && !sy.syncing) {
    syncText += ` | last attempt ${sy.lastAttemptAgoS}s ago (${sy.lastUploaded} uploaded)`;
  }
  syncText += ` | Backend: ${s.activeBackend || 'none reachable yet'}`;
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
    document.getElementById('tzEdit').style.display = match ? 'none' : '';
    document.getElementById('tzCurrent').innerText = match ? '' : `Custom: ${s.tzPosix}`;
  }
}
async function loadSatellites(){
  const r = await fetch('/api/satellites'); const data = await r.json();
  document.getElementById('satSummary').innerText =
    `${data.count} in view, ${data.usedInFix} used in fix`;
  const tbody = document.getElementById('satBody'); tbody.innerHTML = '';
  if (data.satellites.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" class="muted">No satellites in view yet</td></tr>';
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
// Two views in one page, switched by the URL hash (#settings) so the
// browser Back button and bookmarks work without a second copy of the
// page's CSS/JS in flash.
function isSettingsView(){ return location.hash === '#settings'; }
function updateSatPolling(){
  if (!document.hidden && !isSettingsView()) startSatPolling(); else stopSatPolling();
}
function showView(){
  const settings = isSettingsView();
  document.getElementById('mainView').style.display = settings ? 'none' : '';
  document.getElementById('settingsView').style.display = settings ? '' : 'none';
  if (settings) { revertBackendForm(); revertWifiForm(); } // leaving Settings abandons unsaved drafts
  updateSatPolling();
  window.scrollTo(0, 0);
}
window.addEventListener('hashchange', showView);
document.addEventListener('visibilitychange', updateSatPolling);
// Calendar mirrors the backend UI's: month grid, days with tracks
// highlighted (with session count), click a day to list its sessions.
let trackData = {};      // 'YYYY-MM-DD' -> [session files]
let calYear, calMonth;   // displayed month (0-based month)
let selectedDate = null;
async function loadSessions(){
  const r = await fetch('/api/sessions'); const raw = await r.json();
  trackData = Object.fromEntries(Object.entries(raw).filter(([, files]) => files.length));
  const dates = Object.keys(trackData).sort().reverse(); // newest first
  if (!dates.length) {
    document.getElementById('calWrap').style.display = 'none';
    document.getElementById('daySessions').innerHTML = '<span class="status">No tracks recorded yet.</span>';
    return;
  }
  document.getElementById('calWrap').style.display = '';
  let jump = calYear === undefined;
  if (!selectedDate || !trackData[selectedDate]) { selectedDate = dates[0]; jump = true; }
  if (jump) {
    calYear = parseInt(selectedDate.slice(0,4)); calMonth = parseInt(selectedDate.slice(5,7)) - 1;
  }
  renderCalendar();
  renderDay();
}
function renderCalendar(){
  const first = new Date(calYear, calMonth, 1);
  document.getElementById('calTitle').innerText = first.toLocaleString(undefined, {month:'long', year:'numeric'});
  let html = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun'].map(d=>`<div class="cal-dow">${d}</div>`).join('');
  const lead = (first.getDay() + 6) % 7; // Monday-first
  const daysInMonth = new Date(calYear, calMonth + 1, 0).getDate();
  const t = new Date();
  const todayIso = `${t.getFullYear()}-${String(t.getMonth()+1).padStart(2,'0')}-${String(t.getDate()).padStart(2,'0')}`;
  for (let i = 0; i < lead; i++) html += '<div class="cal-day"></div>';
  for (let d = 1; d <= daysInMonth; d++) {
    const iso = `${calYear}-${String(calMonth+1).padStart(2,'0')}-${String(d).padStart(2,'0')}`;
    const files = trackData[iso];
    const cls = ['cal-day','in-month', files ? 'has-tracks' : '',
      iso === selectedDate ? 'selected' : '', iso === todayIso ? 'today' : ''].join(' ');
    html += `<div class="${cls}" ${files ? `onclick="selectDay('${iso}')"` : ''}>`
      + `<span>${d}</span>${files ? `<span class="cal-count">${files.length}</span>` : ''}</div>`;
  }
  document.getElementById('calGrid').innerHTML = html;
}
function shiftMonth(delta){
  calMonth += delta;
  if (calMonth < 0) { calMonth = 11; calYear--; }
  if (calMonth > 11) { calMonth = 0; calYear++; }
  renderCalendar();
}
function selectDay(iso){ selectedDate = iso; renderCalendar(); renderDay(); }
function renderDay(){
  const date = selectedDate, files = trackData[date] || [];
  document.getElementById('daySessions').innerHTML =
    `<div class="date">${date} <a class="btn btn-danger" href="#" onclick="deleteDate('${date}'); return false;" style="font-size:.8rem;font-weight:400;margin-left:.6rem">delete date</a></div>`
    + (files.map(f => `<div class="session"><span>${f}</span><span>
        <a class="btn" href="/api/download?date=${date}&file=${f}">download</a>
        <a class="btn btn-danger" href="#" onclick="deleteSession('${date}','${f}'); return false;" style="margin-left:.6rem">delete</a>
      </span></div>`).join('') || '<span class="status">No sessions for this date.</span>');
}
async function deleteSession(date, file){
  if (!confirm(`Delete ${file} (${date})? This can't be undone.`)) return;
  const r = await fetch(`/api/sessions?date=${encodeURIComponent(date)}&file=${encodeURIComponent(file)}`, {method:'DELETE'});
  if (!r.ok) alert('Not deleted: ' + await r.text()); // e.g. 503 while a sync pass runs
  loadSessions();
}
async function deleteDate(date){
  if (!confirm(`Delete ALL sessions for ${date}? This can't be undone.`)) return;
  const r = await fetch(`/api/sessions?date=${encodeURIComponent(date)}`, {method:'DELETE'});
  if (!r.ok) alert('Not deleted: ' + await r.text());
  loadSessions();
}
async function syncNow(){
  const r = await fetch('/api/sync',{method:'POST'});
  alert(r.ok ? await r.text() : 'Sync not started: ' + await r.text());
  loadStatus();
}

// The form follows the device's saved config (refreshed by every status
// poll) until the user edits a field; from then on it's left alone so the
// 10s poll can't overwrite what's being typed.
let backendFormDirty = false;
['bfHost','bfPort','bfHost2','bfPort2'].forEach(id =>
  document.getElementById(id).addEventListener('input', () => {
    backendFormDirty = true; document.getElementById('bfMsg').innerText = '';
  }));
function fillBackendForm(){
  // Fields stay disabled until the device's current values have arrived:
  // saving a form that was never prefilled would send an empty second
  // host, which the device treats as "remove the second backend".
  if (window._backendHost === undefined) return; // no status received yet
  document.getElementById('bfHost').value = window._backendHost || '';
  document.getElementById('bfPort').value = window._backendPort || 8080;
  document.getElementById('bfHost2').value = window._backendHost2 || '';
  document.getElementById('bfPort2').value = window._backendPort2 || 8080;
  ['bfHost','bfPort','bfHost2','bfPort2','bfSave'].forEach(id => document.getElementById(id).disabled = false);
}
function revertBackendForm(){
  backendFormDirty = false; fillBackendForm();
  document.getElementById('bfMsg').innerText = '';
}
async function saveBackend(){
  const host = document.getElementById('bfHost').value.trim();
  const port = document.getElementById('bfPort').value.trim() || '8080';
  const host2 = document.getElementById('bfHost2').value.trim();
  const port2 = document.getElementById('bfPort2').value.trim() || '8080';
  if (!host) { alert('Primary backend host is required'); return; }
  const url = `/api/backend?host=${encodeURIComponent(host)}&port=${encodeURIComponent(port)}`
            + `&host2=${encodeURIComponent(host2)}&port2=${encodeURIComponent(port2)}`;
  const r = await fetch(url, {method:'POST'});
  if (!r.ok) { alert('Failed: ' + await r.text()); return; }
  backendFormDirty = false;
  document.getElementById('bfMsg').innerText = 'Saved - a sync attempt starts immediately.';
  loadStatus();
}

// Same dirty-draft model as the backend form. Passwords are never sent to
// the browser - the fields stay empty, and the placeholder says whether
// one is saved (empty on save = keep it).
let wifiFormDirty = false;
['wfSsid','wfPass','wfSsid2','wfPass2'].forEach(id =>
  document.getElementById(id).addEventListener('input', () => {
    wifiFormDirty = true; document.getElementById('wfMsg').innerText = '';
  }));
function passPlaceholder(ssid, hasPass){
  if (!ssid) return 'password';
  return hasPass ? 'saved - leave empty to keep' : 'none saved (open network)';
}
function fillWifiForm(){
  const w = window._wifi;
  if (!w) return; // no status received yet - fields stay disabled (see fillBackendForm)
  document.getElementById('wfSsid').value = w.ssid;
  document.getElementById('wfSsid2').value = w.ssid2;
  document.getElementById('wfPass').value = '';
  document.getElementById('wfPass2').value = '';
  document.getElementById('wfPass').placeholder = passPlaceholder(w.ssid, w.hasPass);
  document.getElementById('wfPass2').placeholder = passPlaceholder(w.ssid2, w.hasPass2);
  ['wfSsid','wfPass','wfSsid2','wfPass2','wfSave'].forEach(id => document.getElementById(id).disabled = false);
}
function revertWifiForm(){
  wifiFormDirty = false; fillWifiForm();
  document.getElementById('wfMsg').innerText = '';
}
async function saveWifi(){
  const w = window._wifi || {};
  const ssid = document.getElementById('wfSsid').value;   // not trimmed - spaces are legal in SSIDs
  const pass = document.getElementById('wfPass').value;
  const ssid2 = document.getElementById('wfSsid2').value;
  const pass2 = document.getElementById('wfPass2').value;
  if (!ssid) { alert('Primary network name is required (use Reset WiFi to remove all networks)'); return; }
  for (const [p, label] of [[pass, 'primary'], [pass2, 'second']]) {
    if (p && (p.length < 8 || p.length > 64)) { alert(`The ${label} network password must be 8-63 characters (or a 64-digit hex key)`); return; }
  }
  // A network the device doesn't know yet, saved without a password,
  // would be stored as an open network - make sure that's intended.
  // Saved networks (with or without a password) keep what's stored for them.
  const known = s => s === w.ssid || s === w.ssid2;
  for (const [s, p] of [[ssid, pass], [ssid2, pass2]]) {
    if (s && !p && !known(s) && !confirm(`No password entered for "${s}". Save it as an open network (no password)?`)) return;
  }
  const r = await fetch('/api/wifi', {method:'POST', body: new URLSearchParams({ssid, pass, ssid2, pass2})});
  if (!r.ok) { alert('Failed: ' + await r.text()); return; }
  wifiFormDirty = false;
  document.getElementById('wfMsg').innerText = 'Saved - used from the next connection attempt.';
  loadStatus();
}

async function setReclaim(){
  const box = document.getElementById('stReclaim');
  const r = await fetch(`/api/storage?reclaim=${box.checked ? 1 : 0}`, {method:'POST'});
  if (!r.ok) { alert('Failed: ' + await r.text()); box.checked = !box.checked; }
  loadStatus();
}
async function setDebug(){
  const box = document.getElementById('dbgOn');
  const r = await fetch(`/api/debug?on=${box.checked ? 1 : 0}`, {method:'POST'});
  if (!r.ok) { alert('Failed: ' + await r.text()); box.checked = !box.checked; }
  loadStatus();
}
async function showLog(){
  const view = document.getElementById('dbgView');
  const r = await fetch('/api/log');
  view.textContent = await r.text();
  view.style.display = '';
  view.scrollTop = view.scrollHeight; // newest lines at the bottom
  document.getElementById('dbgShow').innerText = 'Refresh log';
}
async function clearLog(){
  if (!confirm('Delete the stored debug log?')) return;
  await fetch('/api/log/clear', {method:'POST'});
  document.getElementById('dbgView').style.display = 'none';
  document.getElementById('dbgShow').innerText = 'Show log';
  loadStatus();
}

async function shutdownDevice(){
  if (!confirm('Shut the device down? It stops tracking and reacting to movement, and switches back on only when a charger is connected.')) return;
  const btn = document.getElementById('shutdownBtn');
  btn.innerText = 'Shutting down...'; btn.disabled = true;
  let r = null;
  try {
    r = await fetch('/api/shutdown', {method:'POST'});
  } catch (e) { /* device is going down, connection may drop */ }
  if (r && !r.ok) {
    alert('Shutdown refused: ' + await r.text());
    btn.innerText = 'Shut down'; btn.disabled = false;
    return;
  }
  alert('The device is shutting down. Connect a charger to switch it on again.');
}
async function resetWifi(){
  if (!confirm('This erases BOTH saved WiFi networks and reboots the device into setup mode. Backend settings are kept. Continue?')) return;
  const btn = document.getElementById('resetWifiBtn');
  btn.innerText = 'Resetting...'; btn.disabled = true;
  let r = null;
  try {
    r = await fetch('/api/reset-wifi', {method:'POST'});
  } catch (e) { /* device is rebooting, connection may drop mid-response */ }
  if (r && !r.ok) {
    alert('WiFi reset refused: ' + await r.text());
    btn.innerText = 'Reset WiFi'; btn.disabled = false;
    return;
  }
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

showView(); loadStatus(); loadSessions(); setInterval(loadStatus, 10000);
</script></body></html>
)HTML";

// Hostname part of a Host header or an Origin/Referer URL: no scheme, path
// or port, lowercased.
static String hostOnly(String s) {
    int scheme = s.indexOf("://");
    if (scheme >= 0) s = s.substring(scheme + 3);
    int slash = s.indexOf('/');
    if (slash >= 0) s = s.substring(0, slash);
    if (s.startsWith("[")) { // IPv6 literal
        int rb = s.indexOf(']');
        if (rb > 0) s = s.substring(0, rb + 1);
    } else {
        int colon = s.lastIndexOf(':');
        if (colon >= 0) s = s.substring(0, colon);
    }
    s.toLowerCase();
    if (s.endsWith(".")) s.remove(s.length() - 1); // FQDN form "name.lan."
    return s;
}

// Names this device can legitimately be reached by on a LAN: an IP
// literal, a single-label router-DNS name, or a local-only suffix. Anything
// else in Host means DNS rebinding (an attacker's domain re-pointed at the
// device so a web page can read its API).
static bool isLocalHostName(const String &h) {
    // No Host at all: not a browser (browsers always send it) - and the
    // WebServer keeps the previous request's value then anyway.
    if (h.length() == 0) return true;
    IPAddress ip;
    if (ip.fromString(h) || h.startsWith("[")) return true;
    if (h.indexOf('.') < 0) return true;
    // Local-only suffixes (plus ones whose DNS an attacker can't control:
    // ts.net = Tailscale MagicDNS, fritz.box/speedport.ip = router names).
    static const char *const suffixes[] = {".local", ".lan", ".home", ".home.arpa", ".internal",
                                           ".localdomain", ".fritz.box", ".speedport.ip",
                                           ".mshome.net", ".ts.net"};
    for (const char *suf : suffixes) {
        if (h.endsWith(suf)) return true;
    }
    // Site-specific extra suffix (e.g. an office DNS zone), set at build time.
    if (strlen(WEB_EXTRA_HOST_SUFFIX) > 0 && h.endsWith(WEB_EXTRA_HOST_SUFFIX)) return true;
    return false;
}

// No login on this LAN device (by design), but no other WEBSITE may drive
// it either: a page open in any browser on the LAN could otherwise POST to
// /update (multipart is CORS-safelisted, no preflight) and flash arbitrary
// firmware, or point /api/backend at its own server and receive the full
// track history. Browsers always send Origin on cross-site POSTs (and
// "null" from sandboxed frames), so a state-changing request whose Origin
// (or, without one, Referer) names a different host than the device is
// refused. Non-browser clients (curl) send neither and stay allowed.
bool WebUi::requestAllowed(bool mutating, bool respond) {
    String host = hostOnly(_server.hostHeader());
    bool ok = isLocalHostName(host);
    if (ok && mutating) {
        String origin = _server.header("Origin");
        if (origin == "null") {
            ok = false;
        } else {
            if (origin.length() == 0) origin = _server.header("Referer");
            if (origin.length() > 0 && hostOnly(origin) != host) ok = false;
        }
    }
    if (!ok) {
        LOGD("[Web] Refused %s request (Host '%s', Origin '%s')\n",
             mutating ? "state-changing" : "read", host.c_str(), _server.header("Origin").c_str());
        if (respond) {
            _server.send(403, "text/plain", isLocalHostName(host)
                ? "forbidden: request from another website"
                : "forbidden: unknown host name - open the device by its IP address "
                  "(or add your DNS suffix via WEB_EXTRA_HOST_SUFFIX)");
        }
    }
    return ok;
}

void WebUi::begin() {
    static const char *headerKeys[] = {"Origin", "Referer"};
    _server.collectHeaders(headerKeys, 2);

    auto get = [this](const char *path, std::function<void()> h) {
        _server.on(path, HTTP_GET, [this, h]() { _lastRequestMs = millis(); if (requestAllowed(false)) h(); });
    };
    auto change = [this](const char *path, HTTPMethod method, std::function<void()> h) {
        _server.on(path, method, [this, h]() { _lastRequestMs = millis(); if (requestAllowed(true)) h(); });
    };

    get("/", [this]() { handleRoot(); });
    get("/api/sessions", [this]() { handleApiSessions(); });
    get("/api/download", [this]() { handleApiDownload(); });
    get("/api/status", [this]() { handleApiStatus(); });
    get("/api/satellites", [this]() { handleApiSatellites(); });
    get("/api/log", [this]() { handleApiLog(); });
    change("/api/sync", HTTP_POST, [this]() { handleApiSyncNow(); });
    change("/api/sessions", HTTP_DELETE, [this]() { handleApiDelete(); });
    change("/api/reset-wifi", HTTP_POST, [this]() { handleApiResetWifi(); });
    change("/api/shutdown", HTTP_POST, [this]() { handleApiShutdown(); });
    change("/api/timezone", HTTP_POST, [this]() { handleApiSetTimezone(); });
    change("/api/backend", HTTP_POST, [this]() { handleApiSetBackend(); });
    change("/api/wifi", HTTP_POST, [this]() { handleApiSetWifi(); });
    change("/api/log/clear", HTTP_POST, [this]() {
        Log.clear();
        _server.send(200, "text/plain", "cleared");
    });
    change("/api/debug", HTTP_POST, [this]() { handleApiSetDebug(); });
    change("/api/storage", HTTP_POST, [this]() { handleApiSetStorage(); });

    get("/api/ota/status", [this]() {
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
            _lastRequestMs = millis();
            if (!requestAllowed(true)) return; // the upload itself was already refused at its start
            // Success only if handleOtaUpload() actually completed a flash:
            // a refused upload (sync running, begin() failed) never starts
            // Update, so !Update.hasError() alone would read as success and
            // reboot the device on the OLD firmware.
            bool ok = otaLastSuccess;
            _server.sendHeader("Connection", "close");
            _server.send(ok ? 200 : 500, "text/plain",
                         ok ? "OK, rebooting" : (otaLastError.length() ? otaLastError.c_str() : "OTA failed"));
            if (ok) {
                prepareForRestart(); // save the RAM track buffer, continue the trip after the update
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

// date/file arrive straight from the query string and are joined into a
// LittleFS path - without this check "date=..&file=config.json" served (and
// DELETE could remove) files outside /tracks, including config.json with
// the stored WiFi passwords.
static bool isSafePathSegment(const String &s) {
    return s.length() > 0 && s != "." && s.indexOf("..") < 0 && s.indexOf('/') < 0 && s.indexOf('\\') < 0;
}

void WebUi::handleApiDownload() {
    if (!_server.hasArg("date") || !_server.hasArg("file")) {
        _server.send(400, "text/plain", "missing date/file");
        return;
    }
    if (!isSafePathSegment(_server.arg("date")) || !isSafePathSegment(_server.arg("file"))) {
        _server.send(400, "text/plain", "invalid date/file");
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
    doc["backendHost2"] = configManager.get().backendHost2;
    doc["backendPort2"] = configManager.get().backendPort2;
    doc["activeBackend"] = syncManager.isBackendResolved() ? syncManager.activeBackendKey() : String("");
    // WiFi networks: SSIDs and whether a password is stored - never the
    // passwords themselves.
    doc["wifiSsid"] = configManager.get().wifiSsid;
    doc["wifiHasPass"] = configManager.get().wifiPass[0] != '\0';
    doc["wifiSsid2"] = configManager.get().wifiSsid2;
    doc["wifiHasPass2"] = configManager.get().wifiPass2[0] != '\0';
    if (syncManager.isWifiConnected()) {
        doc["wifiConnectedSsid"] = WiFi.SSID();
        doc["wifiRssi"] = WiFi.RSSI();
    } else {
        doc["wifiConnectedSsid"] = nullptr;
        doc["wifiRssi"] = nullptr;
    }
    doc["debugLog"] = Log.enabled();
    doc["reclaimSynced"] = configManager.get().reclaimSynced;
    StorageStatus stor = syncManager.storageStatus();
    JsonObject storage = doc["storage"].to<JsonObject>();
    storage["level"] = stor.level; // 0 ok, 1 almost full, 2 full
    storage["freeKB"] = stor.freeBytes / 1024;
    storage["reclaimableKB"] = stor.reclaimableBytes / 1024;
    storage["unsyncedKB"] = stor.unsyncedBytes / 1024;
    storage["unsyncedFiles"] = stor.unsyncedFiles;
    doc["debugLogKB"] = (unsigned)((Log.storedBytes() + 1023) / 1024);
    doc["fsUsedKB"] = (unsigned)(LittleFS.usedBytes() / 1024);
    doc["fsTotalKB"] = (unsigned)(LittleFS.totalBytes() / 1024);

    doc["gpsFix"] = gpsReader.hasFix();
    doc["gpsSatellites"] = gpsReader.satellites();

    // Sync machinery state - pending files, live progress of a running
    // upload pass (reachable mid-pass because syncPendingSessions services
    // this server between files), and the per-wake-cycle give-up flags.
    SyncStatus ss = syncManager.getStatus();
    JsonObject sync = doc["sync"].to<JsonObject>();
    sync["pending"] = ss.pendingSessions;
    sync["rejected"] = ss.rejected;
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
    // Lifts a give-up pause, retries skipped files and re-probes backends -
    // deferred until the pass ends if one is running (this handler runs
    // between its file uploads).
    bool running = syncManager.isSyncing();
    syncManager.requestManualSync();
    _server.send(202, "text/plain", running ? "sync running - will retry everything once it ends"
                                            : "sync requested");
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
    if (!isSafePathSegment(date) || (_server.hasArg("file") && !isSafePathSegment(_server.arg("file")))) {
        _server.send(400, "text/plain", "invalid date/file");
        return;
    }
    bool ok;
    if (_server.hasArg("file")) {
        ok = trackStore.deleteSession(date, _server.arg("file"));
        syncManager.forgetPath(trackStore.sessionPath(date, _server.arg("file")));
    } else {
        ok = trackStore.deleteDate(date);
        syncManager.forgetDate(date);
    }
    _server.send(ok ? 200 : 404, "text/plain", ok ? "deleted" : "not found or delete failed");
}

void WebUi::handleApiShutdown() {
    if (rejectIfSyncing()) return;
    // The only wake source after a shutdown is the charger pin going HIGH -
    // on a charger right now it would wake again within a second.
    if (powerManager.isCharging()) {
        _server.send(409, "text/plain", "The device is charging - it would switch on again immediately. Unplug it first.");
        return;
    }
    Log.println("[Power] Shutdown requested via web UI");
    _server.send(200, "text/plain", "Shutting down - connect a charger to switch the device on again");
    _server.client().flush();
    delay(200);
    requestShutdown(); // main.cpp closes the trip and sleeps at the next loop pass
}

void WebUi::handleApiResetWifi() {
    if (rejectIfSyncing()) return;
    // Erase only the WiFi SSID/password from the ESP-IDF's own storage -
    // backend host/port/MQTT settings in config.json are untouched, so the
    // captive portal will still prefill them on next setup. On reboot,
    // ConfigManager::isWifiConfigured() sees no saved SSID and main.cpp
    // automatically opens the setup portal (see main.cpp - there's no
    // physical button for this anymore, this link is the only trigger).
    Log.println("[WiFi] Reset requested via web UI - erasing credentials and rebooting");
    configManager.clearWifiNetworks(); // both stored networks
    prepareForRestart();       // save the RAM track buffer, continue the trip
    requestPortalOnNextBoot(); // a planned restart isn't a fresh boot - open the portal explicitly
    _server.send(200, "text/plain", "WiFi credentials cleared - rebooting into setup mode");
    _server.client().flush();
    delay(300);
    // WifiConnector switched the driver to RAM storage, under which the
    // erase below would only clear RAM - the NVS copy must go too.
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
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
    Log.printf("[Config] Timezone set to %s\n", tz.c_str());
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
    // Validate EVERYTHING before touching cfg, so a rejected request never
    // leaves a half-applied config in RAM.
    uint16_t port = cfg.backendPort;
    if (_server.hasArg("port")) {
        long p = _server.arg("port").toInt();
        if (p < 1 || p > 65535) {
            _server.send(400, "text/plain", "invalid port");
            return;
        }
        port = (uint16_t)p;
    }
    // Optional second backend (home/office dual-site setup - see
    // DeviceConfig in config.h). Explicitly EMPTY host2 clears it; args
    // absent leave the stored values untouched.
    bool setHost2 = _server.hasArg("host2");
    String host2 = setHost2 ? _server.arg("host2") : String(cfg.backendHost2);
    host2.trim();
    if (host2.length() >= sizeof(cfg.backendHost2)) {
        _server.send(400, "text/plain", "host2 too long");
        return;
    }
    uint16_t port2 = cfg.backendPort2;
    if (_server.hasArg("port2") && _server.arg("port2").length()) {
        long p2 = _server.arg("port2").toInt();
        if (p2 < 1 || p2 > 65535) {
            _server.send(400, "text/plain", "invalid port2");
            return;
        }
        port2 = (uint16_t)p2;
    }
    strlcpy(cfg.backendHost, host.c_str(), sizeof(cfg.backendHost));
    cfg.backendPort = port;
    strlcpy(cfg.backendHost2, host2.c_str(), sizeof(cfg.backendHost2));
    cfg.backendPort2 = port2;
    configManager.save();
    // The old backend being unreachable says nothing about the new one -
    // clear any give-up state and try immediately, no reboot needed. The
    // sync ledger is deliberately KEPT: it's namespaced per backend
    // ("host:port|path"), so a new address starts with an empty namespace
    // and receives the full history anyway, while unchanged backends keep
    // their records (no pointless re-upload, and SyncManager::reclaimSpace()
    // still knows which files are safely stored somewhere).
    syncManager.resetBackoff();
    syncManager.requestImmediateSync();
    Log.printf("[Config] Backends set to %s:%u / %s:%u via web UI\n",
                  cfg.backendHost, cfg.backendPort,
                  cfg.backendHost2[0] ? cfg.backendHost2 : "-", cfg.backendPort2);
    _server.send(200, "text/plain", "ok");
}

void WebUi::handleApiLog() {
    Log.persist(); // flush the RAM buffer so the files are complete
    if (_server.hasArg("download")) {
        _server.sendHeader("Content-Disposition", "attachment; filename=\"gpslogger-debug.log\"");
    }
    _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server.send(200, "text/plain; charset=utf-8", "");
    bool any = false;
    uint8_t buf[512];
    for (const char *p : {FS_DEBUG_LOG_OLD_PATH, FS_DEBUG_LOG_PATH}) { // oldest first
        if (!LittleFS.exists(p)) continue; // open() on a missing file prints an [E] line
        File f = LittleFS.open(p, "r");
        if (!f) continue;
        size_t n;
        while ((n = f.read(buf, sizeof(buf))) > 0) {
            // A browser that went away mid-transfer would otherwise keep
            // loop() blocked in write timeouts for the rest of the file.
            if (!_server.client().connected()) { f.close(); return; }
            _server.sendContent((const char *)buf, n);
            any = true;
        }
        f.close();
    }
    // Lines persist() couldn't save (flash nearly full) are still in RAM.
    size_t pendingLen;
    const char *pending = Log.buffered(pendingLen);
    if (pendingLen > 0) {
        _server.sendContent("--- not yet saved to flash ---\n");
        _server.sendContent(pending, pendingLen);
        any = true;
    }
    if (!any) {
        _server.sendContent(Log.enabled()
            ? "(debug log is empty so far)\n"
            : "(debug log is off - switch it on in Settings to start recording)\n");
    }
    _server.sendContent(""); // end of chunked response
}

void WebUi::handleApiSetStorage() {
    bool on = _server.arg("reclaim") == "1";
    bool previous = configManager.get().reclaimSynced;
    configManager.get().reclaimSynced = on;
    if (!configManager.save()) {
        configManager.get().reclaimSynced = previous; // keep RAM config and flash in agreement
        _server.send(500, "text/plain", "could not save config");
        return;
    }
    Log.printf("[Config] Delete uploaded tracks when space runs out: %s\n", on ? "on" : "off");
    _server.send(200, "text/plain", on ? "on" : "off");
}

void WebUi::handleApiSetDebug() {
    bool on = _server.arg("on") == "1";
    bool previous = configManager.get().debugLog;
    configManager.get().debugLog = on;
    if (!configManager.save()) {
        configManager.get().debugLog = previous; // keep RAM config and flash in agreement
        _server.send(500, "text/plain", "could not save config");
        return;
    }
    Log.setEnabled(on);
    _server.send(200, "text/plain", on ? "on" : "off");
}

void WebUi::handleApiSetWifi() {
    auto &cfg = configManager.get();
    String ssid = _server.arg("ssid"), ssid2 = _server.arg("ssid2");
    String pass = _server.arg("pass"), pass2 = _server.arg("pass2");
    // SSIDs are NOT trimmed: leading/trailing spaces are legal in an SSID,
    // and trimming would silently rename a saved network (losing its
    // password via the keep-by-SSID lookup below).
    if (ssid.length() == 0) {
        _server.send(400, "text/plain", "primary network name (SSID) is required - use Reset WiFi to remove all networks");
        return;
    }
    if (ssid.length() >= sizeof(cfg.wifiSsid) || ssid2.length() >= sizeof(cfg.wifiSsid2)) {
        _server.send(400, "text/plain", "network name (SSID) too long - max 32 characters");
        return;
    }
    // An empty password field means "keep the password saved for this
    // network" - looked up by SSID, so it still works when the two
    // networks swap places. A network not saved before gets no password
    // (open network).
    auto resolvePass = [&](const String &forSsid, const String &given, String &out) -> bool {
        if (given.length() > 0) {
            if (given.length() < 8 || given.length() >= sizeof(cfg.wifiPass)) return false;
            out = given;
        } else if (forSsid == cfg.wifiSsid) {
            out = cfg.wifiPass;
        } else if (forSsid == cfg.wifiSsid2) {
            out = cfg.wifiPass2;
        } else {
            out = "";
        }
        return true;
    };
    String newPass, newPass2;
    if (!resolvePass(ssid, pass, newPass) || !resolvePass(ssid2, pass2, newPass2)) {
        _server.send(400, "text/plain", "WiFi password must be 8-63 characters (or a 64-digit hex key)");
        return;
    }
    if (ssid2.length() == 0) newPass2 = "";
    strlcpy(cfg.wifiSsid, ssid.c_str(), sizeof(cfg.wifiSsid));
    strlcpy(cfg.wifiPass, newPass.c_str(), sizeof(cfg.wifiPass));
    strlcpy(cfg.wifiSsid2, ssid2.c_str(), sizeof(cfg.wifiSsid2));
    strlcpy(cfg.wifiPass2, newPass2.c_str(), sizeof(cfg.wifiPass2));
    cfg.wifiMigrated = true;
    if (!configManager.save()) {
        _server.send(500, "text/plain", "could not save config");
        return;
    }
    // Takes effect on the next connection attempt - an existing
    // connection is left alone rather than dropping the page mid-save.
    wifiConnector.restartAttempts();
    Log.printf("[Config] WiFi networks set to \"%s\" / \"%s\" via web UI\n",
                  cfg.wifiSsid, cfg.wifiSsid2[0] ? cfg.wifiSsid2 : "-");
    _server.send(200, "text/plain", "ok");
}

void WebUi::handleOtaUpload() {
    HTTPUpload &upload = _server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        _lastRequestMs = millis(); // an upload in progress counts as web UI activity
        if (!requestAllowed(true, false)) { // cross-site: never start writing an image
            otaLastError = "forbidden: cross-site request";
            otaLastSuccess = false;
            return;
        }
        if (syncManager.isSyncing()) {
            // Can't cleanly send a 503 from inside a streaming upload
            // handler; mark the error so the completion handler and
            // /api/ota/status report it, and never call Update.begin().
            otaLastError = "sync in progress - retry OTA in a few seconds";
            otaLastSuccess = false;
            return;
        }
        Log.printf("[OTA] Start: %s\n", upload.filename.c_str());
        otaActive = true;
        otaWrittenBytes = 0;
        otaLastError = "";
        otaLastSuccess = false;
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            otaLastError = "Update.begin() failed - check partition scheme has room for OTA";
            Update.printError(Log);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!Update.isRunning()) return; // begin() refused/never ran - drain the stream silently
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            otaLastError = "Flash write failed mid-upload";
            Update.printError(Log);
        } else {
            otaWrittenBytes += upload.currentSize;
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        otaActive = false;
        if (!Update.isRunning()) return; // begin() refused/never ran - error already recorded
        if (otaLastError.length() == 0 && Update.end(true)) {
            otaLastSuccess = true;
            Log.printf("[OTA] Success, %u bytes\n", upload.totalSize);
        } else if (otaLastError.length()) {
            Update.abort(); // a mid-upload write failed - don't finalize a partial image
        } else {
            otaLastError = "Update.end() failed - image may be corrupt or too large";
            Update.printError(Log);
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        otaActive = false;
        otaLastError = "Upload aborted";
        Update.end(false);
    }
}
