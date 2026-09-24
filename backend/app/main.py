"""
Offline GPS Logger backend.

- REST API for ESP32 devices to PUT GPX session files (grouped by
  device/date) and to POST periodic device status (battery/GPS/...).
- Web UI to browse tracks by calendar date, view them on a map (via the
  built-in tile proxy/cache) and download GPX.
- Designed to run as a Docker container, addressable from Home Assistant
  (either as a standalone container on the HA host's Docker, or as a HA
  add-on using the included config.yaml / Dockerfile).

MQTT was deliberately removed: device status arrives over the same HTTP
channel the uploads already use, and the old MQTT "sync now" command could
never reach a sleeping device anyway (the device syncs automatically
whenever it is awake with WiFi).

Env vars:
  DATA_DIR        - where GPX files are stored (default /data/tracks)
  TILE_CACHE_DIR  - map tile cache (default /data/tile_cache)
  TILE_UPSTREAM   - tile source URL template (see below)
"""
import os
import json
import logging
import shutil
import threading
from pathlib import Path
from datetime import datetime
from typing import Optional

from fastapi import FastAPI, Request, HTTPException, Response
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
import gpxpy

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("gpslogger-backend")

DATA_DIR = Path(os.environ.get("DATA_DIR", "/data/tracks"))
DATA_DIR.mkdir(parents=True, exist_ok=True)

app = FastAPI(title="Offline GPS Logger")

# Bundled static assets (Leaflet for the map preview). Served locally so the
# UI has zero CDN dependencies - it must work on networks where external
# CDNs are blocked and through HA ingress. Only the map TILES still come
# from openstreetmap.org, fetched by the viewer's own browser.
STATIC_DIR = Path(__file__).parent / "static"
if STATIC_DIR.exists():
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

# Last-known status per device, POSTed by the device itself over HTTP
# whenever it is awake with WiFi (see the firmware's postStatusToBackend()).
# In-memory only: it's ephemeral "last seen" telemetry, not archive data.
device_status_cache: dict[str, dict] = {}

# ---------------------------------------------------------------------------
# REST API - device upload endpoint
# ---------------------------------------------------------------------------
def safe_segment(s: str) -> str:
    if not s or "/" in s or ".." in s or "\\" in s:
        raise HTTPException(400, "invalid path segment")
    return s

@app.put("/api/v1/tracks/{device_id}/{date}/{filename}")
async def upload_track(device_id: str, date: str, filename: str, request: Request):
    device_id = safe_segment(device_id)
    date = safe_segment(date)
    filename = safe_segment(filename)
    if not filename.endswith(".gpx"):
        raise HTTPException(400, "only .gpx files accepted")

    try:
        datetime.strptime(date, "%Y-%m-%d")
    except ValueError:
        raise HTTPException(400, "date must be YYYY-MM-DD")

    body = await request.body()
    if not body:
        raise HTTPException(400, "empty body")

    # Validate it's parseable GPX before accepting
    try:
        gpxpy.parse(body.decode("utf-8"))
    except Exception as e:
        raise HTTPException(400, f"invalid GPX: {e}")

    target_dir = DATA_DIR / device_id / date
    target_dir.mkdir(parents=True, exist_ok=True)
    target_path = target_dir / filename
    target_path.write_bytes(body)

    log.info("Stored track %s (%d bytes) for device %s", target_path, len(body), device_id)
    return JSONResponse({"status": "ok", "path": str(target_path.relative_to(DATA_DIR))}, status_code=201)

# ---------------------------------------------------------------------------
# REST API - browsing (used by web UI, and by Home Assistant sensors if desired)
# ---------------------------------------------------------------------------
@app.get("/api/v1/devices")
def list_devices():
    if not DATA_DIR.exists():
        return []
    return sorted([p.name for p in DATA_DIR.iterdir() if p.is_dir()])

@app.get("/api/v1/devices/{device_id}/status")
def device_status(device_id: str):
    return device_status_cache.get(device_id, {"status": "unknown"})

@app.post("/api/v1/devices/{device_id}/status")
async def post_device_status(device_id: str, request: Request):
    """Devices POST their live status here periodically while awake with
    WiFi (battery, charging, GPS fix, board time...). Replaces the former
    MQTT status topic - same payload, same HTTP channel as the uploads."""
    device_id = safe_segment(device_id)
    try:
        payload = json.loads((await request.body()).decode("utf-8"))
    except Exception as e:
        raise HTTPException(400, f"invalid JSON: {e}")
    if not isinstance(payload, dict):
        raise HTTPException(400, "status payload must be a JSON object")
    payload["receivedAt"] = datetime.now().isoformat()
    device_status_cache[device_id] = payload
    return {"status": "ok"}

@app.get("/api/v1/devices/{device_id}/dates")
def list_dates(device_id: str):
    device_id = safe_segment(device_id)
    d = DATA_DIR / device_id
    if not d.exists():
        return []
    return sorted([p.name for p in d.iterdir() if p.is_dir()], reverse=True)

@app.get("/api/v1/devices/{device_id}/dates/{date}/sessions")
def list_sessions(device_id: str, date: str):
    device_id = safe_segment(device_id)
    date = safe_segment(date)
    d = DATA_DIR / device_id / date
    if not d.exists():
        return []
    sessions = []
    for f in sorted(d.glob("*.gpx")):
        stat = f.stat()
        point_count = None
        try:
            with open(f, "r") as fh:
                gpx = gpxpy.parse(fh)
                point_count = sum(len(seg.points) for trk in gpx.tracks for seg in trk.segments)
        except Exception:
            pass
        sessions.append({
            "file": f.name,
            "size_bytes": stat.st_size,
            "modified": datetime.fromtimestamp(stat.st_mtime).isoformat(),
            "points": point_count,
        })
    return sessions

# ---------------------------------------------------------------------------
# Map tile proxy + disk cache. The web UI loads its map tiles from HERE
# (same origin, relative URL - works through HA ingress) instead of hitting
# tile.openstreetmap.org from the browser: corporate proxies/filtered
# networks often block third-party tile requests in the browser ("Access
# blocked" tiles), while the backend host's own internet access works. Each
# tile is fetched from OSM once (with a proper identifying User-Agent, per
# OSM tile policy) and cached on disk forever - tiles of past trips barely
# change, and the cache makes repeat views fast and offline-capable.
# ---------------------------------------------------------------------------
TILE_CACHE_DIR = Path(os.environ.get("TILE_CACHE_DIR", "/data/tile_cache"))
# Upstream tile source ({z}/{x}/{y} placeholders). Overridable so a paid or
# self-hosted tile server can be dropped in without a code change.
# Default is the German OSM tile server rather than tile.openstreetmap.org:
# the main OSM servers block entire networks that violated their usage
# policy at some point, and blocked IPs get an "Access blocked" placeholder
# PNG WITH HTTP 200 for every tile (observed on this deployment's network -
# even direct browser fetches were blocked before this proxy existed).
# tile.openstreetmap.de is separate infrastructure with the same map data
# and a comparable be-gentle policy, which the 2-thread cap + permanent disk
# cache below honors. Standard OSM: TILE_UPSTREAM=https://tile.openstreetmap.org/{z}/{x}/{y}.png
TILE_UPSTREAM = os.environ.get(
    "TILE_UPSTREAM", "https://tile.openstreetmap.de/{z}/{x}/{y}.png")
# OSM tile usage policy (operations.osmfoundation.org/policies/tiles/)
# requirements honored here:
#  - identifying User-Agent (never a browser-imitating one)
#  - at most 2 concurrent download threads to the upstream - a map view
#    fires ~15 tile requests at once, and forwarding that concurrency
#    upstream is precisely what earns the "not following the tile usage
#    policy" block tiles. The semaphore funnels cache misses through 2
#    upstream connections no matter how many browser requests arrive.
#  - heavy caching (tiles are cached on disk indefinitely)
TILE_USER_AGENT = "Offline-GPS-Logger/1.1 (self-hosted personal track archive; cached proxy)"
_tile_fetch_gate = threading.Semaphore(2)

# One-time purge: earlier versions cached upstream responses that turned out
# to be OSM "blocked" placeholder tiles (served with HTTP 200!) - they would
# otherwise be served from cache forever even after switching upstream. The
# marker file keeps this from re-purging on every restart; bump its name
# whenever cached content must be invalidated again.
_tile_cache_marker = TILE_CACHE_DIR / ".v3"
if TILE_CACHE_DIR.exists() and not _tile_cache_marker.exists():
    shutil.rmtree(TILE_CACHE_DIR, ignore_errors=True)
TILE_CACHE_DIR.mkdir(parents=True, exist_ok=True)
_tile_cache_marker.touch(exist_ok=True)

@app.get("/tiles/{z}/{x}/{y}.png")
def map_tile(z: int, x: int, y: int):
    if not (0 <= z <= 19 and 0 <= x < 2 ** z and 0 <= y < 2 ** z):
        raise HTTPException(400, "invalid tile coordinates")
    cached = TILE_CACHE_DIR / str(z) / str(x) / f"{y}.png"
    if not cached.exists():
        import urllib.request
        url = TILE_UPSTREAM.format(z=z, x=x, y=y)
        req = urllib.request.Request(url, headers={"User-Agent": TILE_USER_AGENT})
        with _tile_fetch_gate:
            if not cached.exists():  # another request may have fetched it while we waited
                try:
                    with urllib.request.urlopen(req, timeout=15) as resp:
                        if resp.status != 200:
                            raise HTTPException(502, f"upstream returned {resp.status}")
                        data = resp.read()
                except HTTPException:
                    raise
                except Exception as e:
                    # Never cache failures - propagate and let the client retry
                    raise HTTPException(502, f"tile fetch failed: {e}")
                if not data.startswith(b"\x89PNG"):
                    # Upstream sent something that isn't a PNG tile (an HTML
                    # error page, a policy notice...) - serve nothing and,
                    # critically, cache nothing.
                    raise HTTPException(502, "upstream response is not a PNG tile")
                cached.parent.mkdir(parents=True, exist_ok=True)
                # Atomic publish: write to a temp file and rename into place,
                # so a crash or concurrent request can never leave a
                # truncated tile that the cache would then serve forever.
                tmp = cached.with_suffix(f".tmp-{os.getpid()}-{threading.get_ident()}")
                tmp.write_bytes(data)
                os.replace(tmp, cached)
    return FileResponse(cached, media_type="image/png",
                        headers={"Cache-Control": "public, max-age=2592000"})

@app.get("/api/v1/devices/{device_id}/dates/{date}/sessions/{filename}/points")
def session_points(device_id: str, date: str, filename: str):
    """Track geometry for the web UI's map preview: parsed once server-side
    (the browser shouldn't have to chew through raw GPX XML), downsampled to
    a size that renders instantly even for day-long tracks."""
    device_id = safe_segment(device_id)
    date = safe_segment(date)
    filename = safe_segment(filename)
    path = DATA_DIR / device_id / date / filename
    if not path.exists():
        raise HTTPException(404, "not found")
    try:
        with open(path, "r") as fh:
            gpx = gpxpy.parse(fh)
    except Exception as e:
        raise HTTPException(422, f"unparseable GPX: {e}")

    pts = [(p.latitude, p.longitude)
           for trk in gpx.tracks for seg in trk.segments for p in seg.points]
    total = len(pts)
    # Downsample evenly but always keep the final point, so the track's end
    # stays exact on the map.
    MAX_POINTS = 2000
    if total > MAX_POINTS:
        step = total / MAX_POINTS
        pts = [pts[int(i * step)] for i in range(MAX_POINTS - 1)] + [pts[-1]]

    time_bounds = gpx.get_time_bounds()
    return {
        "points": [[round(lat, 6), round(lon, 6)] for lat, lon in pts],
        "totalPoints": total,
        "distance_m": round(gpx.length_2d() or 0),
        "start": time_bounds.start_time.isoformat() if time_bounds.start_time else None,
        "end": time_bounds.end_time.isoformat() if time_bounds.end_time else None,
    }

@app.get("/api/v1/devices/{device_id}/dates/{date}/sessions/{filename}")
def download_session(device_id: str, date: str, filename: str):
    device_id = safe_segment(device_id)
    date = safe_segment(date)
    filename = safe_segment(filename)
    path = DATA_DIR / device_id / date / filename
    if not path.exists():
        raise HTTPException(404, "not found")
    return FileResponse(path, media_type="application/gpx+xml", filename=filename)

@app.delete("/api/v1/devices/{device_id}/dates/{date}/sessions/{filename}")
def delete_session(device_id: str, date: str, filename: str):
    """Delete one recorded track. Also removes the date/device directories
    once they're left empty, so the calendar and device list don't keep
    showing entries with nothing left in them."""
    device_id = safe_segment(device_id)
    date = safe_segment(date)
    filename = safe_segment(filename)
    path = DATA_DIR / device_id / date / filename
    if not path.exists():
        raise HTTPException(404, "not found")
    path.unlink()
    log.info("Deleted track %s", path)

    date_dir = path.parent
    try:
        next(date_dir.iterdir())
    except StopIteration:
        date_dir.rmdir()
        device_dir = date_dir.parent
        try:
            next(device_dir.iterdir())
        except StopIteration:
            device_dir.rmdir()

    return {"status": "ok"}

# ---------------------------------------------------------------------------
# Simple web UI - calendar browser (separate from the device's own mini-UI)
# ---------------------------------------------------------------------------
# NOTE for Home Assistant ingress: every URL in this page (fetch() calls and
# download hrefs) MUST be relative, never starting with "/". Under ingress
# the app is served at /api/hassio_ingress/<token>/, so absolute paths like
# /api/v1/... resolve against HA's own root and 404. Relative paths resolve
# against the ingress base automatically (ingress always serves the page at
# a URL ending in "/"), and they still work identically when the backend is
# accessed directly on port 8080.
INDEX_HTML = """
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GPS Logger - Track Archive</title>
<link rel="stylesheet" href="static/leaflet/leaflet.min.css">
<script src="static/leaflet/leaflet.min.js"></script>
<style>
/* Light theme (matches HA's default light UI) */
:root{--bg:#f4f6f8;--card:#ffffff;--line:#e3e7ec;--fg:#1f2733;--dim:#66707d;
  --accent:#1a6fd4;--ok:#2e7d4f}
body{font-family:system-ui,sans-serif;margin:0;padding:1rem;background:var(--bg);
  color:var(--fg);max-width:900px;margin-inline:auto}
h1{font-size:1.2rem;margin:.4rem 0 1rem}
select,button{background:#fff;color:var(--fg);border:1px solid #c9d1da;border-radius:6px;
  padding:.45rem .7rem;font-size:.9rem}
button:hover{border-color:var(--accent);cursor:pointer}
.status{font-size:.85rem;color:var(--dim);margin-top:.5rem}
.card{background:var(--card);border-radius:10px;padding:1rem;margin-top:1rem;
  border:1px solid var(--line);box-shadow:0 1px 2px rgba(16,24,40,.05)}
/* Calendar constrained to ~75% of the card and centered - full card width
   made the day cells needlessly large on desktop. */
.cal-head,.cal-grid{max-width:630px;margin-left:auto;margin-right:auto}
.cal-head{display:flex;justify-content:space-between;align-items:center;margin-bottom:.6rem}
.cal-title{font-weight:600}
.cal-grid{display:grid;grid-template-columns:repeat(7,1fr);gap:4px}
.cal-dow{text-align:center;font-size:.7rem;color:var(--dim);padding:.2rem 0}
.cal-day{aspect-ratio:1.4;display:flex;flex-direction:column;align-items:center;
  justify-content:center;border-radius:8px;font-size:.85rem;color:var(--dim);
  border:1px solid transparent;position:relative}
.cal-day.in-month{color:var(--fg)}
.cal-day.has-tracks{background:#e3efff;color:#144a86;cursor:pointer;border-color:#bcd8f6}
.cal-day.has-tracks:hover{border-color:var(--accent)}
.cal-day.selected{border-color:var(--accent);background:#cfe4fc}
.cal-day.today{outline:1px dashed #a6b0bc}
.cal-count{font-size:.6rem;color:var(--accent);line-height:1;margin-top:2px}
.session{display:flex;justify-content:space-between;align-items:center;gap:.6rem;
  padding:.55rem 0;border-top:1px solid var(--line);flex-wrap:wrap}
.session:first-of-type{border-top:none}
.sess-meta{font-size:.8rem;color:var(--dim)}
a.btn{color:var(--accent);text-decoration:none}
a.btn-danger{color:#c0392b}
.empty{color:var(--dim);font-style:italic}
#mapCard{display:none}
#map{height:420px;border-radius:8px}
.map-meta{font-size:.8rem;color:var(--dim);margin-top:.5rem}
</style></head><body>
<h1>GPS Logger - Track Archive</h1>
<div class="status" id="deviceStatus"></div>
<div class="card" id="calCard" style="display:none">
  <div class="cal-head">
    <button onclick="shiftMonth(-1)">&#9664;</button>
    <span class="cal-title" id="calTitle"></span>
    <button onclick="shiftMonth(1)">&#9654;</button>
  </div>
  <div class="cal-grid" id="calGrid"></div>
</div>
<div class="card" id="sessions"><span class="empty">Loading...</span></div>
<div class="card" id="mapCard">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:.5rem">
    <span style="font-weight:600" id="mapTitle"></span>
    <a class="btn" href="#" onclick="closeMap(); return false;">close &#10005;</a>
  </div>
  <div id="map"></div>
  <div class="map-meta" id="mapMeta"></div>
</div>
<script>
// All request URLs are RELATIVE (no leading slash) - required for HA ingress.
const api = (path, opts) => fetch(path, opts);

let trackDates = new Map(); // 'YYYY-MM-DD' -> session count (count lazily filled)
let calYear, calMonth;      // currently displayed month (0-based month)
let selectedDate = null;

let currentDevice = null; // single-device setup - set once by loadDevices()
function device(){ return currentDevice; }

async function loadDevices(){
  const r = await api('api/v1/devices'); const devices = await r.json();
  if (!devices.length) {
    // Fresh install: no device has ever synced. Showing a blank calendar
    // here reads as "broken" - show one clear onboarding message instead
    // and keep the calendar hidden until data exists.
    document.getElementById('calCard').style.display = 'none';
    document.getElementById('sessions').innerHTML =
      '<span class="empty">No devices have synced yet. Tracks appear here automatically ' +
      'after the first upload - the device syncs whenever it is awake with WiFi ' +
      '(after movement, or while charging).</span>';
    return;
  }
  // Single-device setup by design: no selector, just use the first device.
  // (Additional device IDs would still be reachable via the REST API.)
  currentDevice = devices[0];
  document.getElementById('calCard').style.display = '';
  const r2 = await api(`api/v1/devices/${device()}/dates`);
  const dates = await r2.json(); // sorted newest first
  trackDates = new Map(dates.map(d => [d, null]));
  // Open the calendar on the most recent month that has tracks
  const latest = dates.length ? dates[0] : new Date().toISOString().slice(0,10);
  calYear = parseInt(latest.slice(0,4)); calMonth = parseInt(latest.slice(5,7)) - 1;
  selectedDate = dates.length ? dates[0] : null;
  renderCalendar();
  if (selectedDate) {
    loadSessions(selectedDate);
  } else {
    document.getElementById('sessions').innerHTML =
      '<span class="empty">No tracks recorded yet - they appear automatically ' +
      'after the first synced session.</span>';
  }
  loadDeviceStatus();
}

async function loadDeviceStatus(){
  const el = document.getElementById('deviceStatus');
  try {
    const r = await api(`api/v1/devices/${device()}/status`);
    const s = await r.json();
    if (s.battery === undefined) { el.innerText = `${device()}: no status received yet (the device reports whenever it is awake with WiFi)`; return; }
    el.innerText = `${device()}: battery ${s.battery}%${s.charging ? (s.battery >= 100 ? ' (fully charged)' : ' (charging)') : ''}`
      + ` | GPS: ${s.gpsFix ? 'fix' : 'no fix'} (${s.gpsSatellites ?? '?'} sats)`
      + ` | FW ${s.fw ?? '?'}${s.boardTimeLocal ? ' | board time ' + s.boardTimeLocal : ''}`;
  } catch (e) { el.innerText = ''; }
}

function renderCalendar(){
  const grid = document.getElementById('calGrid');
  const title = document.getElementById('calTitle');
  const first = new Date(calYear, calMonth, 1);
  title.innerText = first.toLocaleString(undefined, {month:'long', year:'numeric'});
  const dows = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];
  let html = dows.map(d=>`<div class="cal-dow">${d}</div>`).join('');
  // Monday-first offset for the 1st of the month
  const lead = (first.getDay() + 6) % 7;
  const daysInMonth = new Date(calYear, calMonth + 1, 0).getDate();
  const todayIso = new Date().toISOString().slice(0,10);
  for (let i = 0; i < lead; i++) html += '<div class="cal-day"></div>';
  for (let d = 1; d <= daysInMonth; d++) {
    const iso = `${calYear}-${String(calMonth+1).padStart(2,'0')}-${String(d).padStart(2,'0')}`;
    const has = trackDates.has(iso);
    const cls = ['cal-day','in-month', has ? 'has-tracks' : '',
      iso === selectedDate ? 'selected' : '', iso === todayIso ? 'today' : ''].join(' ');
    const count = trackDates.get(iso);
    html += `<div class="${cls}" ${has ? `onclick="selectDay('${iso}')"` : ''}>`
      + `<span>${d}</span>${has ? `<span class="cal-count">${count ?? '●'}</span>` : ''}</div>`;
  }
  grid.innerHTML = html;
}

function shiftMonth(delta){
  calMonth += delta;
  if (calMonth < 0) { calMonth = 11; calYear--; }
  if (calMonth > 11) { calMonth = 0; calYear++; }
  renderCalendar();
}

function selectDay(iso){
  selectedDate = iso;
  renderCalendar();
  loadSessions(iso);
}

async function loadSessions(date){
  const el = document.getElementById('sessions');
  el.innerHTML = '<span class="empty">Loading...</span>';
  const r = await api(`api/v1/devices/${device()}/dates/${date}/sessions`);
  const sessions = await r.json();
  trackDates.set(date, sessions.length);
  renderCalendar();
  el.innerHTML = `<div style="font-weight:600;margin-bottom:.4rem">${date}</div>`
    + (sessions.map(s => `
      <div class="session">
        <span>${s.file}<div class="sess-meta">${s.points ?? '?'} points · ${(s.size_bytes/1024).toFixed(1)} KB · ${s.modified.replace('T',' ').slice(0,16)}</div></span>
        <span>
          <a class="btn" href="#" onclick="showMap('${date}','${s.file}'); return false;">map</a>
          <a class="btn" style="margin-left:.7rem" href="api/v1/devices/${device()}/dates/${date}/sessions/${s.file}" download>download</a>
          <a class="btn btn-danger" style="margin-left:.7rem" href="#" onclick="deleteSession('${date}','${s.file}'); return false;">delete</a>
        </span>
      </div>`).join('')
      || '<span class="empty">No sessions for this date.</span>');
}

async function deleteSession(date, file){
  if (!confirm(`Delete track "${file}" from ${date}? This can't be undone.`)) return;
  const r = await api(`api/v1/devices/${device()}/dates/${date}/sessions/${file}`, {method:'DELETE'});
  if (!r.ok) { alert('Could not delete track: ' + await r.text()); return; }
  if (map && document.getElementById('mapTitle').innerText === file) closeMap();

  // Re-pull the date list: deleting the day's last session removes the
  // whole date directory server-side, and the calendar needs to reflect
  // that (cell should stop being clickable/highlighted).
  const r2 = await api(`api/v1/devices/${device()}/dates`);
  const dates = await r2.json();
  trackDates = new Map(dates.map(d => [d, null]));

  if (trackDates.has(date)) {
    loadSessions(date);
  } else {
    selectedDate = dates.length ? dates[0] : null;
    renderCalendar();
    if (selectedDate) {
      loadSessions(selectedDate);
    } else {
      document.getElementById('sessions').innerHTML =
        '<span class="empty">No tracks recorded yet - they appear automatically ' +
        'after the first synced session.</span>';
    }
  }
}

// --- Map preview (Leaflet + OSM tiles; both load in the VIEWER's browser,
// so this works through HA ingress as long as the viewer has internet -
// the HA host itself never needs to reach the tile servers) ---
let map = null, trackLayer = null;
function closeMap(){
  document.getElementById('mapCard').style.display = 'none';
}
async function showMap(date, file){
  const r = await api(`api/v1/devices/${device()}/dates/${date}/sessions/${file}/points`);
  if (!r.ok) { alert('Could not load track: ' + await r.text()); return; }
  const t = await r.json();
  if (!t.points.length) { alert('Track has no points.'); return; }

  document.getElementById('mapCard').style.display = 'block';
  document.getElementById('mapTitle').innerText = file;
  if (!map) {
    map = L.map('map');
    // Tiles come from the backend's own /tiles proxy+cache (relative URL,
    // ingress-safe), NOT directly from openstreetmap.org - browsers behind
    // filtering proxies get "Access blocked" tiles otherwise, and the
    // backend-side cache makes revisits fast/offline. Data still (c) OSM.
    // The ?v= suffix busts browser caches: earlier versions served OSM
    // "Access blocked" placeholder tiles with a 30-day max-age, so browsers
    // keep showing them from local cache even after the server was fixed.
    // Bump the value whenever previously-served tile content must be
    // invalidated for all viewers.
    L.tileLayer('tiles/{z}/{x}/{y}.png?v=2', {
      maxZoom: 19,
      attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
    }).addTo(map);
  }
  if (trackLayer) trackLayer.remove();
  trackLayer = L.layerGroup().addTo(map);
  const line = L.polyline(t.points, {color:'#5ab0ff', weight:4, opacity:.9}).addTo(trackLayer);
  L.circleMarker(t.points[0], {radius:6, color:'#4ea36f', fillColor:'#4ea36f', fillOpacity:1})
    .bindTooltip('Start').addTo(trackLayer);
  L.circleMarker(t.points[t.points.length-1], {radius:6, color:'#e05a5a', fillColor:'#e05a5a', fillOpacity:1})
    .bindTooltip('End').addTo(trackLayer);
  map.fitBounds(line.getBounds(), {padding:[24,24]});

  const km = (t.distance_m/1000).toFixed(2);
  let dur = '';
  if (t.start && t.end) {
    const mins = Math.round((new Date(t.end) - new Date(t.start)) / 60000);
    dur = ` · ${Math.floor(mins/60)}h ${mins%60}min`;
  }
  document.getElementById('mapMeta').innerText =
    `${km} km${dur} · ${t.totalPoints} points` +
    (t.points.length < t.totalPoints ? ` (map shows ${t.points.length})` : '');
  document.getElementById('mapCard').scrollIntoView({behavior:'smooth'});
}

loadDevices();
</script></body></html>
"""

@app.get("/", response_class=HTMLResponse)
def index():
    return INDEX_HTML

@app.get("/healthz")
def healthz():
    return {"status": "ok"}