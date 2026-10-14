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
  TILE_CACHE_DIR  - map tile cache (default /data/tile_cache). Must be a
                    DEDICATED directory - never DATA_DIR or a parent of it
                    (the cache purge refuses to run in that case).
  TILE_UPSTREAM   - tile source URL template (see below)
"""
import os
import re
import json
import time
import asyncio
import logging
import tempfile
import threading
import contextlib
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
# device_id -> (payload as POSTed + server receive timestamp,
#               time.monotonic() at receive). The age shown in the UI is
# computed from the monotonic clock: the Pi has no RTC, so wall-clock time
# can jump (NTP sync after boot) and would make ages wrong.
device_status_cache: dict[str, tuple[dict, float]] = {}
STATUS_MAX_BYTES = 16 * 1024  # the device's payload is a few hundred bytes

# ---------------------------------------------------------------------------
# Path-segment validation. Device ids, dates and file names become
# filesystem paths AND are echoed back to the web UI - which, under HA
# ingress, runs on the Home Assistant origin (a crafted name rendered as
# markup there could steal HA auth tokens). The upload/status endpoints are
# unauthenticated, so every endpoint taking these segments accepts ONLY
# these strict allow-lists (rejecting, never "sanitizing"), and the listing
# endpoints skip anything on disk that doesn't match (e.g. stored before
# this validation existed). The firmware's own names - "gpslog-a1b2c3",
# "2026-08-22", "2026-08-22_14-03-11.gpx" - all fit.
# [0-9] rather than \d: \d also matches non-ASCII Unicode digits.
# ---------------------------------------------------------------------------
_DEVICE_ID_RE = re.compile(r"[A-Za-z0-9_-]{1,64}")
_DATE_RE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}")
_FILENAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,80}\.gpx")

def is_valid_device_id(s: str) -> bool:
    return _DEVICE_ID_RE.fullmatch(s) is not None

def is_valid_date(s: str) -> bool:
    if _DATE_RE.fullmatch(s) is None:
        return False
    try:
        datetime.strptime(s, "%Y-%m-%d")  # also rejects e.g. 2026-02-30
    except ValueError:
        return False
    return True

def is_valid_filename(s: str) -> bool:
    return _FILENAME_RE.fullmatch(s) is not None

def check_device_id(s: str) -> str:
    if not is_valid_device_id(s):
        raise HTTPException(400, "invalid device id (1-64 of A-Z a-z 0-9 _ -)")
    return s

def check_date(s: str) -> str:
    if not is_valid_date(s):
        raise HTTPException(400, "date must be YYYY-MM-DD")
    return s

def check_filename(s: str) -> str:
    if not is_valid_filename(s):
        raise HTTPException(400, "invalid file name (expected <name>.gpx, "
                                 "name of A-Z a-z 0-9 _ . - starting with a letter/digit)")
    return s

def _fsync_dir(path: Path) -> None:
    """Persist directory-entry changes (a new file name, a rename) on POSIX.
    Directories can't be opened like this on Windows - no-op there."""
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)

def store_track_durably(target_path: Path, data: bytes) -> None:
    """Write data to target_path so that it is on stable storage before the
    caller returns 201: the device marks a file as synced on 201 and may
    later delete its own copy, so an upload acknowledged but lost to a power
    cut (Pi on an SD card...) would be gone for good. Temp file in the SAME
    directory (os.replace is only atomic within one filesystem), flush +
    fsync, atomic rename onto the target, then fsync the directory entries.
    A crash at any point leaves either the old file or the complete new one,
    never a truncated GPX."""
    target_dir = target_path.parent
    new_dir = not target_dir.exists()
    target_dir.mkdir(parents=True, exist_ok=True)
    # Hidden ".<name>.<random>.tmp" - never matches the "*.gpx" listing
    fd, tmp_name = tempfile.mkstemp(dir=target_dir, prefix=f".{target_path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp_name, 0o644)  # mkstemp creates 0600; match normal file perms
        os.replace(tmp_name, target_path)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp_name)
        raise
    _fsync_dir(target_dir)
    if new_dir:
        # mkdir(parents=True) may have created the date and device dirs too
        _fsync_dir(target_dir.parent)
        _fsync_dir(DATA_DIR)

# ---------------------------------------------------------------------------
# REST API - device upload endpoint
# ---------------------------------------------------------------------------

GPX_HEADER = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<gpx version="1.1" creator="esp32c6-gps-logger" '
    'xmlns="http://www.topografix.com/GPX/1/1">\n'
    " <trk>\n  <name>{name}</name>\n  <trkseg>\n"
)
GPX_FOOTER = "  </trkseg>\n </trk>\n</gpx>\n"

def repair_truncated_gpx(text: str, filename: str) -> str | None:
    """Salvage a device GPX damaged by a reset mid-write: the header or the
    footer (or the edge of a trackpoint) can be missing. Keeps every
    complete <trkpt>...</trkpt> and rebuilds the envelope around them.
    Returns None when no complete trackpoint survives."""
    start = text.find("<trkpt")
    end = text.rfind("</trkpt>")
    if start < 0 or end < start:
        return None
    points = text[start:end + len("</trkpt>")]
    candidate = GPX_HEADER.format(name=filename.removesuffix(".gpx")) + "   " + points + "\n" + GPX_FOOTER
    try:
        gpx = gpxpy.parse(candidate)
    except Exception:
        return None
    if not any(seg.points for trk in gpx.tracks for seg in trk.segments):
        return None
    return candidate

@app.put("/api/v1/tracks/{device_id}/{date}/{filename}")
async def upload_track(device_id: str, date: str, filename: str, request: Request):
    device_id = check_device_id(device_id)
    date = check_date(date)
    if not filename.endswith(".gpx"):
        raise HTTPException(400, "only .gpx files accepted")
    filename = check_filename(filename)

    body = await request.body()
    if not body:
        raise HTTPException(400, "empty body")

    # Validate it's parseable GPX before accepting
    try:
        text = body.decode("utf-8", errors="replace")
        gpxpy.parse(text)
    except Exception as e:
        repaired = repair_truncated_gpx(text, filename)
        if repaired is None:
            raise HTTPException(400, f"invalid GPX: {e}")
        body = repaired.encode("utf-8")
        log.warning("Repaired damaged GPX %s/%s/%s (%s)", device_id, date, filename, e)

    target_path = DATA_DIR / device_id / date / filename
    # Only acknowledge (201) once the file is durably on disk - see
    # store_track_durably(). Runs in a worker thread: fsync on an SD card
    # can take a while and must not stall the event loop.
    await asyncio.to_thread(store_track_durably, target_path, body)

    log.info("Stored track %s (%d bytes) for device %s", target_path, len(body), device_id)
    return JSONResponse({"status": "ok", "path": str(target_path.relative_to(DATA_DIR))}, status_code=201)

# ---------------------------------------------------------------------------
# REST API - browsing (used by web UI, and by Home Assistant sensors if desired)
# ---------------------------------------------------------------------------
@app.get("/api/v1/devices")
def list_devices():
    if not DATA_DIR.exists():
        return []
    return sorted([p.name for p in DATA_DIR.iterdir()
                   if p.is_dir() and is_valid_device_id(p.name)])

@app.get("/api/v1/devices/{device_id}/status")
def device_status(device_id: str):
    device_id = check_device_id(device_id)
    entry = device_status_cache.get(device_id)
    if entry is None:
        return {"status": "unknown"}
    payload, received_mono = entry
    out = dict(payload)
    # Seconds since the backend RECEIVED this status, computed server-side
    # so the UI's "updated N min ago" doesn't depend on the viewer's clock
    # agreeing with the server's.
    out["ageSeconds"] = max(0, round(time.monotonic() - received_mono))
    return out

@app.post("/api/v1/devices/{device_id}/status")
async def post_device_status(device_id: str, request: Request):
    """Devices POST their live status here periodically while awake with
    WiFi (battery, charging, GPS fix, board time...). Replaces the former
    MQTT status topic - same payload, same HTTP channel as the uploads."""
    device_id = check_device_id(device_id)
    body = await request.body()
    if len(body) > STATUS_MAX_BYTES:
        raise HTTPException(413, "status payload too large")
    try:
        payload = json.loads(body.decode("utf-8"))
    except Exception as e:
        raise HTTPException(400, f"invalid JSON: {e}")
    if not isinstance(payload, dict):
        raise HTTPException(400, "status payload must be a JSON object")
    # Server-side receive timestamps (overwriting any same-named device keys):
    # receivedAt is local time WITH its UTC offset, receivedAtEpoch is Unix
    # seconds - both for API consumers (HA sensors...); the UI uses the
    # ageSeconds computed on GET.
    payload["receivedAt"] = datetime.now().astimezone().isoformat()
    payload["receivedAtEpoch"] = round(time.time(), 3)
    device_status_cache[device_id] = (payload, time.monotonic())
    return {"status": "ok"}

@app.get("/api/v1/devices/{device_id}/dates")
def list_dates(device_id: str):
    device_id = check_device_id(device_id)
    d = DATA_DIR / device_id
    if not d.exists():
        return []
    return sorted([p.name for p in d.iterdir() if p.is_dir() and is_valid_date(p.name)],
                  reverse=True)

@app.get("/api/v1/devices/{device_id}/dates/{date}/sessions")
def list_sessions(device_id: str, date: str):
    device_id = check_device_id(device_id)
    date = check_date(date)
    d = DATA_DIR / device_id / date
    if not d.exists():
        return []
    sessions = []
    for f in sorted(d.glob("*.gpx")):
        if not f.is_file() or not is_valid_filename(f.name):
            continue
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
# The purge deletes ONLY things shaped exactly like cached tiles - never an
# rmtree of TILE_CACHE_DIR: that dir is an env var, and pointed at (a parent
# of) DATA_DIR - e.g. TILE_CACHE_DIR=/share/gpslogger with the add-on's
# DATA_DIR=/share/gpslogger/tracks - an rmtree would wipe the GPX archive.
# As a second guard it refuses to run at all when the tile dir is DATA_DIR
# or contains it.
_TILE_NUM_RE = re.compile(r"[0-9]+")
# <y>.png, plus <y>.tmp-<pid>-<thread> left over by an interrupted write in
# map_tile() below
_TILE_FILE_RE = re.compile(r"[0-9]+\.(?:png|tmp-[0-9]+-[0-9]+)")

def _tile_dir_contains_archive() -> bool:
    tiles = TILE_CACHE_DIR.resolve()
    return DATA_DIR.resolve().is_relative_to(tiles)  # true for equal paths too

def purge_tile_cache(root: Path) -> int:
    """Delete cached tile files <root>/<z>/<x>/<y>.png (z, x, y all numeric)
    and then the z/x directories that became empty. Anything else - other
    files, non-numeric directories, symlinks - is left untouched, and
    nothing outside root is ever visited. Returns the number of files
    deleted."""
    removed = 0
    for zdir in root.iterdir():
        if zdir.is_symlink() or not zdir.is_dir() or not _TILE_NUM_RE.fullmatch(zdir.name):
            continue
        for xdir in zdir.iterdir():
            if xdir.is_symlink() or not xdir.is_dir() or not _TILE_NUM_RE.fullmatch(xdir.name):
                continue
            for f in xdir.iterdir():
                if f.is_symlink() or not f.is_file() or not _TILE_FILE_RE.fullmatch(f.name):
                    continue
                f.unlink()
                removed += 1
            with contextlib.suppress(OSError):
                xdir.rmdir()  # fails (kept) if something non-tile is inside
        with contextlib.suppress(OSError):
            zdir.rmdir()
    return removed

_tile_cache_marker = TILE_CACHE_DIR / ".v3"
TILE_CACHE_DIR.mkdir(parents=True, exist_ok=True)
if not _tile_cache_marker.exists():
    if _tile_dir_contains_archive():
        # No marker written: the purge is still owed once the cache is moved
        # to a dedicated directory (it'll be empty there anyway).
        log.error("TILE_CACHE_DIR %s is or contains DATA_DIR %s - refusing to purge the "
                  "tile cache; point TILE_CACHE_DIR at a dedicated directory",
                  TILE_CACHE_DIR, DATA_DIR)
    else:
        try:
            n = purge_tile_cache(TILE_CACHE_DIR)
            if n:
                log.info("Purged %d cached tiles from %s (cache format change)", n, TILE_CACHE_DIR)
            _tile_cache_marker.touch(exist_ok=True)
        except OSError as e:
            # Not fatal for serving; the marker stays unwritten so the next
            # start retries.
            log.warning("Tile cache purge in %s failed: %s", TILE_CACHE_DIR, e)

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
    device_id = check_device_id(device_id)
    date = check_date(date)
    filename = check_filename(filename)
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
    device_id = check_device_id(device_id)
    date = check_date(date)
    filename = check_filename(filename)
    path = DATA_DIR / device_id / date / filename
    if not path.exists():
        raise HTTPException(404, "not found")
    return FileResponse(path, media_type="application/gpx+xml", filename=filename)

@app.delete("/api/v1/devices/{device_id}/dates/{date}/sessions/{filename}")
def delete_session(device_id: str, date: str, filename: str):
    """Delete one recorded track. Also removes the date/device directories
    once they're left empty, so the calendar and device list don't keep
    showing entries with nothing left in them."""
    device_id = check_device_id(device_id)
    date = check_date(date)
    filename = check_filename(filename)
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
.top{display:flex;justify-content:space-between;align-items:center;gap:.6rem;
  flex-wrap:wrap;margin:.4rem 0 1rem}
.top h1{margin:0}
select,button{background:#fff;color:var(--fg);border:1px solid #c9d1da;border-radius:6px;
  padding:.45rem .7rem;font-size:.9rem}
button:hover{border-color:var(--accent);cursor:pointer}
.status{font-size:.85rem;color:var(--dim);margin-top:.5rem}
.status.stale{color:#b26a00}
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
<div class="top">
  <h1>GPS Logger - Track Archive</h1>
  <!-- Only shown when more than one device id exists (see loadDevices) -->
  <select id="deviceSelect" aria-label="Device" style="display:none"></select>
</div>
<div class="status" id="deviceStatus"></div>
<div class="card" id="calCard" style="display:none">
  <div class="cal-head">
    <button id="prevMonth">&#9664;</button>
    <span class="cal-title" id="calTitle"></span>
    <button id="nextMonth">&#9654;</button>
  </div>
  <div class="cal-grid" id="calGrid"></div>
</div>
<div class="card" id="sessions"><span class="empty">Loading...</span></div>
<div class="card" id="mapCard">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:.5rem">
    <span style="font-weight:600" id="mapTitle"></span>
    <a class="btn" href="#" id="closeMap">close &#10005;</a>
  </div>
  <div id="map"></div>
  <div class="map-meta" id="mapMeta"></div>
</div>
<script>
// All request URLs are RELATIVE (no leading slash) - required for HA ingress.
const api = (path, opts) => fetch(path, opts);

// SECURITY: device ids, dates, file names and device-reported status fields
// are untrusted - anyone who can reach the unauthenticated upload/status
// endpoints controls them - and under HA ingress this page runs on the Home
// Assistant origin, where injected markup could read HA's auth tokens. So
// they are NEVER interpolated into HTML strings or inline handlers: the DOM
// is built with h() below (text nodes, setAttribute, addEventListener) and
// every URL path segment goes through encodeURIComponent (apiPath()). The
// backend additionally rejects/hides names outside strict allow-lists.
function h(tag, props, ...children){
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (typeof v === 'function' && k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'class') e.className = v;
    else e.setAttribute(k, v === true ? '' : String(v));
  }
  for (const c of children) {
    if (c === null || c === undefined || c === false) continue;
    e.append(c instanceof Node ? c : String(c)); // strings become TEXT nodes
  }
  return e;
}
const apiPath = (...segs) => 'api/v1/' + segs.map(s => encodeURIComponent(String(s))).join('/');
async function getJson(path){
  const r = await api(path, {cache: 'no-store'});
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}
function showMessage(text){
  document.getElementById('sessions').replaceChildren(h('span', {class: 'empty'}, text));
}
const NO_TRACKS_MSG = 'No tracks recorded yet - they appear automatically ' +
  'after the first synced session.';
const pad2 = n => String(n).padStart(2, '0');
// The browser's LOCAL calendar date. (toISOString() is the UTC date - off
// by one day around midnight for any non-UTC viewer.)
const localIsoDate = d => `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;

let trackDates = new Map(); // 'YYYY-MM-DD' -> session count (count lazily filled)
let calYear, calMonth;      // currently displayed month (0-based month)
let selectedDate = null;

let currentDevice = null; // device whose tracks are shown - set by loadDevices()/the selector
let knownDevices = [];
function device(){ return currentDevice; }

async function loadDevices(preferred){
  const devices = await getJson(apiPath('devices'));
  knownDevices = devices;
  const sel = document.getElementById('deviceSelect');
  if (!devices.length) {
    // Fresh install: no device has ever synced. Showing a blank calendar
    // here reads as "broken" - show one clear onboarding message instead
    // and keep the calendar hidden until data exists.
    currentDevice = null;
    sel.style.display = 'none';
    document.getElementById('calCard').style.display = 'none';
    showMessage('No devices have synced yet. Tracks appear here automatically ' +
      'after the first upload - the device syncs whenever it is awake with WiFi ' +
      '(after movement, or while charging).');
    loadDeviceStatus(); // clears the status line
    return;
  }
  // Default to the device that recorded MOST RECENTLY (newest track date),
  // not simply devices[0]: ids sort alphabetically, so a board that came
  // back with a new id after a reflash/replacement would never be shown.
  const lists = await Promise.all(devices.map(d =>
    getJson(apiPath('devices', d, 'dates')).catch(() => [])));
  const datesOf = new Map(devices.map((d, i) => [d, lists[i]])); // each sorted newest first
  const newest = d => datesOf.get(d)[0] || '';
  let pick = devices[0];
  if (datesOf.has(preferred)) pick = preferred;
  else for (const d of devices) if (newest(d) > newest(pick)) pick = d;
  // Selector only when there is a real choice: the normal single-device
  // setup deliberately shows no dropdown.
  sel.replaceChildren(...devices.map(d =>
    h('option', {value: d}, newest(d) ? `${d} (last track ${newest(d)})` : `${d} (no tracks)`)));
  sel.value = pick;
  sel.style.display = devices.length > 1 ? '' : 'none';
  showDevice(pick, datesOf.get(pick));
}

function showDevice(id, dates){
  currentDevice = id;
  closeMap();
  document.getElementById('calCard').style.display = '';
  trackDates = new Map(dates.map(d => [d, null])); // dates sorted newest first
  // Open the calendar on the most recent month that has tracks
  const latest = dates.length ? dates[0] : localIsoDate(new Date());
  calYear = parseInt(latest.slice(0,4)); calMonth = parseInt(latest.slice(5,7)) - 1;
  selectedDate = dates.length ? dates[0] : null;
  renderCalendar();
  if (selectedDate) {
    loadSessions(selectedDate);
  } else {
    showMessage(NO_TRACKS_MSG);
  }
  loadDeviceStatus();
}

document.getElementById('deviceSelect').addEventListener('change', async ev => {
  const sel = ev.target, id = sel.value;
  try {
    const dates = await getJson(apiPath('devices', id, 'dates'));
    // showDevice() switches currentDevice; responses still in flight for
    // the previous device then see device() changed and are discarded.
    if (sel.value === id) showDevice(id, dates);
  } catch (e) {
    if (sel.value === id) showMessage('Could not load tracks: ' + e.message);
  }
});

// Device status is only POSTed while the device is awake with WiFi, so the
// last one can be hours old: re-poll it and always say how old it is.
const STATUS_REFRESH_MS = 30000;
const STATUS_STALE_S = 600;
function formatAge(sec){
  if (sec < 60) return 'just now';
  if (sec < 3600) return `${Math.floor(sec / 60)} min ago`;
  if (sec < 86400) return `${Math.floor(sec / 3600)} h ago`;
  return `${Math.floor(sec / 86400)} d ago`;
}
async function loadDeviceStatus(){
  const el = document.getElementById('deviceStatus');
  const dev = device();
  const show = (text, stale, title) => {
    el.textContent = text; el.classList.toggle('stale', stale); el.title = title || '';
  };
  if (!dev) { show('', false); return; }
  try {
    const s = await getJson(apiPath('devices', dev, 'status'));
    if (dev !== device()) return; // device switched while this was in flight
    if (s.battery === undefined) {
      show(`${dev}: no status received yet (the device reports whenever it is awake with WiFi)`, false);
      return;
    }
    let text = `${dev}: battery ${s.battery}%${s.charging ? (s.battery >= 100 ? ' (fully charged)' : ' (charging)') : ''}`
      + ` | GPS: ${s.gpsFix ? 'fix' : 'no fix'} (${s.gpsSatellites ?? '?'} sats)`
      + ` | FW ${s.fw ?? '?'}${s.boardTimeLocal ? ' | board time ' + s.boardTimeLocal : ''}`;
    let stale = false;
    if (typeof s.ageSeconds === 'number') {
      stale = s.ageSeconds > STATUS_STALE_S;
      text += ` | updated ${formatAge(s.ageSeconds)}${stale ? ' (stale)' : ''}`;
    }
    show(text, stale, s.receivedAt ? `Received by the backend at ${s.receivedAt}` : '');
  } catch (e) { if (dev === device()) show('', false); }
}
setInterval(() => { if (!document.hidden) loadDeviceStatus(); }, STATUS_REFRESH_MS);
document.addEventListener('visibilitychange', () => { if (!document.hidden) loadDeviceStatus(); });

function renderCalendar(){
  const grid = document.getElementById('calGrid');
  const title = document.getElementById('calTitle');
  const first = new Date(calYear, calMonth, 1);
  title.textContent = first.toLocaleString(undefined, {month:'long', year:'numeric'});
  const dows = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];
  const cells = dows.map(d => h('div', {class: 'cal-dow'}, d));
  // Monday-first offset for the 1st of the month
  const lead = (first.getDay() + 6) % 7;
  const daysInMonth = new Date(calYear, calMonth + 1, 0).getDate();
  const todayIso = localIsoDate(new Date());
  for (let i = 0; i < lead; i++) cells.push(h('div', {class: 'cal-day'}));
  for (let d = 1; d <= daysInMonth; d++) {
    const iso = `${calYear}-${pad2(calMonth + 1)}-${pad2(d)}`;
    const has = trackDates.has(iso);
    const cls = ['cal-day','in-month', has ? 'has-tracks' : '',
      iso === selectedDate ? 'selected' : '', iso === todayIso ? 'today' : ''].join(' ');
    const count = trackDates.get(iso);
    cells.push(h('div', {class: cls, onclick: has ? () => selectDay(iso) : null},
      h('span', null, d), has ? h('span', {class: 'cal-count'}, count ?? '●') : null));
  }
  grid.replaceChildren(...cells);
}

function shiftMonth(delta){
  calMonth += delta;
  if (calMonth < 0) { calMonth = 11; calYear--; }
  if (calMonth > 11) { calMonth = 0; calYear++; }
  renderCalendar();
}
document.getElementById('prevMonth').addEventListener('click', () => shiftMonth(-1));
document.getElementById('nextMonth').addEventListener('click', () => shiftMonth(1));

function selectDay(iso){
  selectedDate = iso;
  renderCalendar();
  loadSessions(iso);
}

// onclick handler for a "#" link: runs fn without navigating
const linkAction = fn => ev => { ev.preventDefault(); fn(); };

async function loadSessions(date){
  const el = document.getElementById('sessions');
  const dev = device();
  showMessage('Loading...');
  let sessions;
  try {
    sessions = await getJson(apiPath('devices', dev, 'dates', date, 'sessions'));
  } catch (e) {
    if (dev === device() && date === selectedDate) showMessage('Could not load sessions: ' + e.message);
    return;
  }
  if (dev !== device() || date !== selectedDate) return; // superseded by a newer selection
  trackDates.set(date, sessions.length);
  renderCalendar();
  const rows = sessions.map(s => h('div', {class: 'session'},
    h('span', null, s.file,
      h('div', {class: 'sess-meta'},
        `${s.points ?? '?'} points · ${(s.size_bytes/1024).toFixed(1)} KB · ${String(s.modified).replace('T',' ').slice(0,16)}`)),
    h('span', null,
      h('a', {class: 'btn', href: '#', onclick: linkAction(() => showMap(date, s.file))}, 'map'),
      ' ',
      h('a', {class: 'btn', style: 'margin-left:.7rem', download: true,
              href: apiPath('devices', dev, 'dates', date, 'sessions', s.file)}, 'download'),
      ' ',
      h('a', {class: 'btn btn-danger', style: 'margin-left:.7rem', href: '#',
              onclick: linkAction(() => deleteSession(date, s.file))}, 'delete'))));
  el.replaceChildren(h('div', {style: 'font-weight:600;margin-bottom:.4rem'}, date),
    ...(rows.length ? rows : [h('span', {class: 'empty'}, 'No sessions for this date.')]));
}

async function deleteSession(date, file){
  if (!confirm(`Delete track "${file}" from ${date}? This can't be undone.`)) return;
  const dev = device();
  const r = await api(apiPath('devices', dev, 'dates', date, 'sessions', file), {method:'DELETE'});
  if (!r.ok) { alert('Could not delete track: ' + await r.text()); return; }
  if (mapTrack === trackKey(dev, date, file)) closeMap();

  // Re-pull the date list: deleting the day's last session removes the
  // whole date directory server-side, and the calendar needs to reflect
  // that (cell should stop being clickable/highlighted).
  const dates = await getJson(apiPath('devices', dev, 'dates'));
  if (dev !== device()) return;
  if (!dates.length && knownDevices.length > 1) {
    // That was this device's last track, so its directory is gone too:
    // refresh the selector and fall back to the most recent other device.
    loadDevices();
    return;
  }
  trackDates = new Map(dates.map(d => [d, null]));

  if (trackDates.has(date)) {
    loadSessions(date);
  } else {
    selectedDate = dates.length ? dates[0] : null;
    renderCalendar();
    if (selectedDate) {
      loadSessions(selectedDate);
    } else {
      showMessage(NO_TRACKS_MSG);
    }
  }
}

// --- Map preview (Leaflet + OSM tiles; both load in the VIEWER's browser,
// so this works through HA ingress as long as the viewer has internet -
// the HA host itself never needs to reach the tile servers) ---
let map = null, trackLayer = null;
let mapTrack = null; // trackKey() of the track currently shown on the map
const trackKey = (dev, date, file) => [dev, date, file].join('/');
const nextFrame = () => new Promise(resolve => requestAnimationFrame(() => resolve()));
function closeMap(){
  document.getElementById('mapCard').style.display = 'none';
  mapTrack = null;
}
document.getElementById('closeMap').addEventListener('click', linkAction(closeMap));
async function showMap(date, file){
  const dev = device();
  const r = await api(apiPath('devices', dev, 'dates', date, 'sessions', file, 'points'));
  if (!r.ok) { alert('Could not load track: ' + await r.text()); return; }
  const t = await r.json();
  if (dev !== device()) return;
  if (!t.points.length) { alert('Track has no points.'); return; }

  document.getElementById('mapCard').style.display = 'block';
  document.getElementById('mapTitle').textContent = file;
  mapTrack = trackKey(dev, date, file);
  // Let the now-visible card get laid out before Leaflet measures it.
  await nextFrame();
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
  // Leaflet caches the container size. A window resize while the card was
  // display:none made it store 0x0, and fitBounds on a 0x0 map picks
  // maxZoom (19) -> grey/wrong tiles. Re-measure BEFORE fitting.
  map.invalidateSize();
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
  document.getElementById('mapMeta').textContent =
    `${km} km${dur} · ${t.totalPoints} points` +
    (t.points.length < t.totalPoints ? ` (map shows ${t.points.length})` : '');
  document.getElementById('mapCard').scrollIntoView({behavior:'smooth'});
}

loadDevices().catch(e => showMessage('Could not load devices: ' + e.message));
</script></body></html>
"""

@app.get("/", response_class=HTMLResponse)
def index():
    return INDEX_HTML

@app.get("/healthz")
def healthz():
    return {"status": "ok"}