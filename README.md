# Offline GPS Logger

Battery-powered GPS tracker built on ESP32-C6 + UART GPS module, with local
RAM buffering, periodic flash persistence as GPX, WiFi auto-sync to up to
two Raspberry Pi / Home Assistant backends over REST (tracks + live
status) through up to two WiFi networks, a captive-portal WiFi/config
manager, OTA firmware updates, a switchable debug log, and a simplified
on-device web UI plus a fuller backend web UI for browsing tracks by
calendar date. Power is managed aggressively: motion-interrupt deep sleep,
an 80 MHz CPU, no WiFi while rolling and short WiFi windows only when
parked, the GPS switched off when it can't get a fix, and a **Shut down**
mode that keeps the device off until a charger is connected (see
[Power consumption](#power-consumption)).

## Step-by-step setup

Follow this in order for a first-time build. Reference sections with full
detail (pin table, API, tuning notes) are further down.

### Part 1 — Hardware wiring

1. Connect the GPS module (ATGM336H) to the ESP32-C6:
   - GPS TX → ESP32-C6 D9 (GPIO20, the ESP's RX)
   - GPS RX → ESP32-C6 D10 (GPIO18, the ESP's TX)
   - **GPS VCC → Drain of Q1 (AO3401 P-MOSFET)**, not straight to 3.3V -
     main VCC is switched, not left always-on. Q1's Source → 3.3V, Gate →
     ESP32-C6 D5 (GPIO23) through a 150Ω series resistor (R1), with a 10KΩ
     pull-up (R2) from the gate node to 3.3V so the switch defaults OFF if
     the GPIO is left floating (e.g. during boot before pinMode() runs).
     Pin 5 (ON/OFF) is not used in this build. Backup power for the
     RTC/SRAM domain comes from a separate pin (VBAT, pin 6), which the
     breakout backs with its own small battery - **on this build that
     battery does not hold the receiver's state**: every power-on is a
     cold start (3-5.5 min to the first fix in practice, see TODO). Wiring
     VBAT to the XIAO's always-on 3V3 instead is the planned fix. See Wiring
     notes below.
2. Connect the ADXL345 accelerometer (I2C):
   - SDA → D7 (GPIO17), SCL → D8 (GPIO19), INT1 → D2 (GPIO2)
   - VCC → 3.3V, GND → GND (leave INT2 unconnected)
   - SDA/SCL are grouped with the GPS wiring (D9/D10) on the same side of
     the board for short wire runs. INT1 can't join them, though - it has
     to stay on D2 (see the Wiring notes below for why).
3. **Battery + charging:** solder a 3.7V LiPo's leads directly to the
   BAT+ / BAT- pads on the underside of the XIAO ESP32-C6 (positive near
   the "D5" silkscreen marking, negative near "D8" - check your specific
   board revision's silkscreen). Its built-in power management
   chip handles charge/discharge automatically, no external TP4056 or
   similar charge IC needed. Feed the Qi wireless receiver module's 5V
   output into the XIAO's 5V pin (in parallel with USB-C) so it charges
   from the pad the same way it would from a cable. **Required:** a
   resistor divider from the 5V pin to D0 (GPIO0) for charger detection
   (see Wiring notes below - the XIAO has no dedicated CHRG pin). Don't
   leave D0 unconnected: the firmware relies on it to skip deep sleep while
   charging and to wake on plug-in, and a floating pin reads randomly -
   the device then either never sleeps or wakes itself in a loop.
4. **Battery voltage sense:** wire a voltage divider from battery+ to D1
   (GPIO1/A1, ADC). Use two similar-value resistors (e.g. 100k/100k) so the
   divided voltage stays under 3.3V at full charge — note the exact ratio,
   it's needed in step 6.

### Part 2 — Firmware setup

5. Install PlatformIO (VS Code extension, or CLI: `pip install platformio`).
6. Calibrate battery constants in `firmware/include/config.h`:
   - Measure your actual divider ratio and adjust `BATTERY_DIVIDER_RATIO`.
   - Battery % comes from a built-in LiPo curve (3.20V = 0%, 4.15V =
     100%, `volts[]`/`pcts[]` in `src/power_manager.cpp`), applied to the
     divider-corrected battery voltage - edit that table if your cell
     differs. (`BATTERY_ADC_MAX_MV` / `BATTERY_ADC_MIN_MV` in `config.h`
     are no longer used.)
   - `ADXL_I2C_ADDR` defaults to `0x53` (SDO/ALT-ADDR tied low). If your
     breakout ties it high, the firmware finds the sensor at `0x1D` anyway
     and logs a hint - set `ADXL_I2C_ADDR` to `0x1D` to silence it.
7. Build and flash:
   ```bash
   cd firmware
   pio run -t upload
   pio device monitor
   ```
   **The XIAO ESP32-C6 needs manual bootloader entry before every upload**
   (not just crash recovery) - it uses native USB with no CP2102/CH340
   bridge chip, and unlike Espressif's own DevKits, Seeed's XIAO boards
   don't wire up the classic DTR/RTS auto-reset circuit that lets
   PlatformIO silently reset the chip into bootloader mode. So each time
   you run `pio run -t upload`, do this first:
   1. Press and hold the **BOOT** button.
   2. While still holding BOOT, tap **RST** once (or plug in the USB
      cable if it wasn't already connected).
   3. Release BOOT after ~1 second.
   4. Now run `pio run -t upload` - it should connect immediately.
   5. **Once the upload reports success, press RST once more - just RST,
      do NOT hold BOOT this time.** The same missing auto-reset circuitry
      that requires manual bootloader entry also means the chip won't
      automatically leave bootloader mode and boot your new firmware after
      flashing - it'll otherwise just sit there silently in the ROM
      bootloader, which looks identical to a hung/crashed app (no boot
      banner, nothing on serial, no WiFi/AP) but isn't one. This plain-RST
      step is what actually starts your firmware running.

   If you see `missing SConscript file '.../pioarduino-build.py'`, PlatformIO
   has a stale/mismatched platform package cached from a previous project.
   `pio pkg uninstall` calls out to the package registry over the network
   and can hang - skip it and just delete the cached directories directly
   (no network needed), along with this project's local build cache, then
   rebuild so it re-fetches fresh copies matching the corrected
   `platformio.ini`:
   ```bash
   rm -rf ~/.platformio/platforms/espressif32 ~/.platformio/packages/framework-arduinoespressif32
   rm -rf .pio
   pio run -t upload
   ```
   (If a `pio pkg uninstall` is already stuck when you read this, `Ctrl+C`
   to kill it first - it's safe to interrupt, nothing has been deleted yet -
   then run the commands above instead.)

   If the build instead hangs at `Resolving esp32-c6-devkitm-1
   dependencies...`, PlatformIO is stuck talking to the network - either
   downloading the pioarduino platform zip (a sizeable GitHub release
   asset) or hitting PlatformIO's own registry/telemetry API. Diagnose with:
   ```bash
   pio run -v -t upload   # verbose - watch for the last line before it freezes
   ```
   - Test the download path directly: `curl -L -o /tmp/test.zip --progress-bar
     https://github.com/pioarduino/platform-espressif32/releases/download/54.03.20/platform-espressif32.zip`.
     If this also stalls, your network is blocking GitHub's release CDN
     (`objects.githubusercontent.com`) even though `github.com` itself may
     be reachable - a common corporate/restrictive-network issue.
   - If it's PlatformIO's own registry/telemetry call hanging, disable
     telemetry and retry: `pio settings set enable_telemetry No`.
   - If it's the GitHub CDN being blocked, download the zip manually via a
     browser and point `platformio.ini` at the local file instead of the
     URL (see the comment in `platformio.ini` for the exact syntax).

   If the build instead gets past downloading everything (platform, tools,
   libraries all install fine) and then fails with `No module named pip` /
   `CalledProcessError: ... python -m pip list ...` while processing
   `arduino.py`, PlatformIO's own internal Python virtual environment
   (`~/.platformio/penv`) is missing `pip` - often left in a broken,
   partially-set-up state by an earlier interrupted/stalled install (e.g.
   the network stall above). Repair it, no network required:
   ```bash
   ~/.platformio/penv/bin/python -m ensurepip --upgrade
   ~/.platformio/penv/bin/python -m pip --version   # confirm it worked
   pio run -t upload
   ```
   If `ensurepip` isn't available, bootstrap pip manually instead:
   ```bash
   curl https://bootstrap.pypa.io/get-pip.py -o /tmp/get-pip.py
   ~/.platformio/penv/bin/python /tmp/get-pip.py
   ```
   As a last resort, rebuild the whole virtual environment from scratch:
   ```bash
   rm -rf ~/.platformio/penv
   curl -fsSL -o /tmp/get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
   python3 /tmp/get-platformio.py
   pio run -t upload
   ```

   **If the port connects fine but nothing ever prints** (no ROM bootloader
   banner, no `[Boot #1]` line, total silence) after an upload reports
   success, you're almost certainly still sitting in the ROM bootloader -
   see step 5 above: press RST once *without* holding BOOT to actually
   boot the newly-flashed firmware.

   **If the serial port disappears entirely instead** (`ls /dev/cu.*` shows
   nothing, upload/monitor can't find the device even after doing the
   manual bootloader-entry steps above), the board is likely mid-crash or
   boot-looping in a way that's also blocking the USB peripheral itself.
   Try the same BOOT+RST sequence again, holding BOOT slightly longer
   (~2 seconds) after plugging back in, and check `ls /dev/cu.*` before
   releasing.

   **If the port connects and reconnects in a tight repeating loop**
   (`Reconnecting... Connected!` over and over, `Disconnected (read failed:
   ...)`) **with zero output ever appearing** - not even the ROM
   bootloader's `rst:0x..`/`SPIWP:0x..` lines, which print independent of
   any app-level crash - that's a brownout reset happening before anything
   can run, not a firmware bug as such. A full chip erase (`pio run -t
   erase`) + reflash rules out stale flash state as the cause; if it still
   crash-loops with zero output afterward, the likely cause is several
   peripherals (GPS, ADXL345, buzzer) drawing startup current
   simultaneously on a marginal supply.

   The real fix, per Espressif's own guidance, is addressing the power
   supply itself, not disabling the brownout detector - on a genuinely
   underpowered board, disabling it often just trades a clean reset-loop
   for undefined/corrupted crashes instead of actually fixing anything:
   - Use a supply/battery path rated for at least 500mA.
   - Add a capacitor (e.g. 100-470µF) across 3.3V/GND near the power input
     to absorb startup current spikes.
   - Keep power wiring short and reasonably thick; check for bad solder
     joints, especially anywhere touched during this project's many pin
     reassignments.
   - If you can disconnect peripherals, do that first and reconnect them
     one at a time to isolate which one/wire is responsible.

   If you can't disconnect anything (e.g. everything's soldered down) and
   want to confirm it's actually a brownout before doing hardware work:
   uncomment `-D DISABLE_BROWNOUT_DETECTOR=1` in `platformio.ini`
   temporarily and reflash - if the device boots normally with it
   disabled, that confirms brownout as the cause (if it still doesn't
   boot, brownout is ruled out and something else is going on). **Remove
   that flag again afterward** - it disables real flash-corruption
   protection and isn't a substitute for the hardware fixes above. Note
   this calls `esp_brownout_disable()` (the portable API for RISC-V chips
   like the C6, which don't have the classic Xtensa `RTC_CNTL_BROWN_OUT_REG`
   register at all) from ESP-IDF's `esp_private` namespace - if it fails to
   *link* (not just compile) as `undefined reference to
   esp_brownout_disable`, the precompiled Arduino core libraries this
   toolchain uses don't export that symbol, and this diagnostic isn't
   available; skip straight to the hardware fixes above instead.

   **If the crash dump itself is visible** (register dump, backtrace with
   file/line info, `Rebooting...` repeating) rather than total silence,
   that's a real, decodable app-level crash rather than a brownout - read
   the `assert failed:` / exception line directly. One specific one worth
   calling out: `assert failed: lfs_fs_grow_ lfs.c:5238 (block_count >=
   lfs->block_count)` means LittleFS found a filesystem superblock on flash
   left over from a *previous, differently-sized* `spiffs` partition (see
   the warning comment in `partitions.csv` - any edit to that file changes
   the physical size of the LittleFS region, and needs a full chip erase
   afterward, not just a normal reflash, or exactly this happens). This is
   a hard C assert inside the mount routine itself, so it crashes before
   our own format-on-fail recovery logic ever gets a chance to run - fix
   is `pio run -t erase` then reflash.

   If autodetect ever picks the wrong port (or can't find it) even when one
   is visible in `ls /dev/cu.*`, set it explicitly - see the commented-out
   `upload_port`/`monitor_port` lines in `platformio.ini`.

   **If the port instead reconnects fine but keeps dropping/reappearing
   repeatedly** while you're actively developing over USB, check the D0
   charger-detect divider first: with it wired, USB power counts as
   charging and the device never deep-sleeps (after 7 minutes still it
   only ends the session). Without it, D0 floats and the device can wake
   itself in a loop or sleep after 7 minutes with no motion - and the
   XIAO's native USB CDC fully resets on every deep-sleep wake cycle. If you also see
   `I2C transaction unexpected nack detected` in the log, that's just the
   ADXL345 not responding (harmless if it isn't wired up yet - the driver's
   own error logging is silenced by default now, so if you still see it,
   you're on an older build). For a stable connection while bench-testing,
   uncomment the `[env:esp32-c6-devkitm-1-dev]` block at the end of
   `platformio.ini` (it ships commented out) - it disables deep sleep and
   adds raw NMEA / ADXL-noise debug output - then build with:
   ```bash
   pio run -e esp32-c6-devkitm-1-dev -t upload
   ```
   Before deploying it battery-powered in the field, flash the normal env
   explicitly (`pio run -e esp32-c6-devkitm-1 -t upload`) and comment the
   block out again: with no `default_envs`, a bare `pio run -t upload`
   builds and uploads both envs, the `-dev` one last.
8. First boot: the device can't join WiFi yet, so it opens a captive portal
   AP named `GPSLogger-Setup-XXXX`.

### Part 3 — Backend setup

Do this before or during step 8, so you have a backend IP/port to enter.

9. Deploy the backend (Docker or Home Assistant App ecosystem):
   - **Standalone Docker on a Pi:**
     ```bash
     cd backend
     docker compose up -d --build
     ```
     This gives a backend on port 8080.
   - **Home Assistant Local App installation:** 
     Ensure your user account has **Administrator** privileges (Advanced Mode is permanently active on all admin profiles starting with version 2026.6). Copy the entire `backend/` folder into your system's `/addons/` directory using an ecosystem utility like **Samba share**, **Studio Code Server**, or **File Editor**.
10. Note the backend's IP/hostname and REST port (8080 by default) — these
    get entered into the captive portal next.

### Part 4 — Connect the device to WiFi + backend

11. On your phone/laptop, connect to the `GPSLogger-Setup-XXXX` WiFi network.
12. A captive portal page should open automatically (or browse to
    `192.168.4.1`).
13. Choose your WiFi network and enter its password.
14. Fill in the extra fields: backend host/IP and REST port (8080).
15. Save — the device joins your WiFi. The network becomes the device's
    **primary network**. A second network (e.g. office next to home) and
    a second backend are added later in the on-device **Settings** page
    (see Part 5) - the device then tries both, no switching needed.

    **If it accepts the credentials but never actually connects** (serial
    monitor shows `[Config] WARNING: WiFi did not actually connect`), this
    is a known class of issue with the underlying WiFiManager library and
    almost always comes down to one of:
    - **Router is 5GHz-only or band-steering** - the ESP32-C6 radio only
      supports 2.4GHz. If your SSID is shared across both bands, create a
      separate 2.4GHz-only SSID (temporarily, if needed) and connect the
      logger to that.
    - **WPA3-only or PMF-required security** - try switching the router to
      WPA2/WPA3-mixed (or plain WPA2) instead of WPA3-only, and turn off
      "PMF required" if your router has that option.
    - **Wrong password / typo** - a failed connect attempt during initial
      setup keeps the portal open automatically, so just retry entering it.
    - Watch `pio device monitor` while it connects - `wm.setDebugOutput`
      is enabled, so you'll see the actual `*wm:` connect attempt logs and
      the underlying ESP-IDF error code, which usually points straight at
      the cause.

    **To change WiFi later**:
    - *While the device is reachable*: open `http://<device-ip>/#settings`
      → **WiFi**, edit the primary and/or second network, **Save**. Leave a
      password field empty to keep the saved password. **Reset WiFi** on
      the same card erases both networks (backend settings are kept) and
      reboots straight into the setup portal.
    - *At a new location where neither saved network exists*: put the
      device on the charger and leave it still. Once it has been still for
      10 minutes and unable to reach any known network for 3, it reboots
      into the setup portal once (`GPSLogger-Setup-XXXX`, open for 3
      minutes; a mistyped password can simply be retried). The network is
      saved only after the device actually connected to it, and is
      **added** - it fills a free slot, or replaces the network that did
      not connect most recently.
    - *Last resort* (device unreachable and the portal doesn't come up):
      full erase and reflash:
      ```bash
      pio run -t erase   # wipes flash, including the saved WiFi credentials
      pio run -t upload
      ```
      This also wipes any logged-but-unsynced GPX sessions on the device.

### Part 5 — Verify it's working

16. Find the device's IP (check your router's client list, or watch serial
    monitor output).
17. Open `http://<device-ip>/` in a browser — you should see the on-device
    UI: battery %, charging state, WiFi/sync status (**Sync now**), flash
    usage, satellites, a red banner if storage is almost full, and (once
    you've moved around) a month calendar of GPX sessions to download or
    delete. Everyday info only - configuration lives on the **Settings**
    page (`⚙ Settings` link, or `http://<device-ip>/#settings`): backends,
    timezone, WiFi networks, storage (delete uploaded tracks when space
    runs out), power (**Shut down**), debug log, and firmware update. The
    page is light by day (07:00-19:00) and dark otherwise.
18. Open the backend UI at `http://<backend-ip>:8080/` (or via the Home
    Assistant sidebar) — browse the calendar, view tracks on the map,
    download GPX, and check the live device-status line (reported by the
    device over HTTP whenever it is awake with WiFi).
19. Take the device for a walk/drive — confirm points log (every ≥3s, ≥5m
    movement), and that a flush hits flash after 15 min (also when the
    2000-point RAM buffer fills, before sleep/restart, and at the start of
    an upload pass - **Sync now** only runs one if a file is pending).
    With **Record debug log** on, every GPS power-on is followed by
    `[GPS] First fix N s after power-on` - tens of seconds is a cold
    start, a few seconds a hot start (working backup cell).
20. Unplug it first - on USB/charger power it never deep-sleeps (after 7
    min still it only ends the session and powers GPS down). Tick
    **Record debug log** in Settings, leave the device stationary for 7+
    minutes with no vibration, then tap/move it: it should wake at once via
    the motion interrupt rather than waiting on a timer. **Show log** then
    has `[Motion] No ADXL activity for timeout period - sleeping` followed
    by `[Boot] Woke via ADXL345 motion interrupt`. Then test **Settings →
    Power → Shut down** (on battery - it is refused while charging): the
    device must ignore movement until a charger is plugged in, then boot
    normally (`[Power] Shutting down ...` in the log, then a boot woken
    via the charger).
21. On battery (the buzzer stays silent while charging), drain (or
    simulate, by editing `LOW_BATTERY_PERCENT` temporarily) a low battery
    condition and confirm the buzzer beeps 5 times, repeating once a minute
    until charged or critical.

### Part 6 — OTA updates going forward

22. Build a new firmware binary:
    ```bash
    pio run
    ```
    The output lands at `.pio/build/esp32-c6-devkitm-1/firmware.bin`.
23. Push it over WiFi instead of USB - either:
    - Open `http://<device-ip>/#settings`, use the "Firmware update (OTA)"
      card, pick the `.bin` file, and hit Flash (shows upload/flash
      progress, then a verification check and reboot countdown), or
    - Script it directly:
      ```bash
      curl -F "firmware=@.pio/build/esp32-c6-devkitm-1/firmware.bin" http://<device-ip>/update
      ```

## Repo layout

```
firmware/                 PlatformIO project (ESP32-C6)
  platformio.ini
  partitions.csv
  include/*.h
  src/*.cpp
backend/                  FastAPI backend - Docker only, doubles as the HA App
  app/main.py
  app/requirements.txt    pinned deps (installed from wheels/)
  app/static/leaflet/     bundled Leaflet - the map UI needs no CDN
  Dockerfile              offline install from wheels/ (works for both paths)
  docker-compose.yml      standalone `docker compose up -d --build` path
  config.yaml             HA local app manifest - copy this whole folder into
                           HA's /addons/ directory to run it as a custom App
  wheels/                 vendored deps (aarch64 only) + update-wheels.sh to
                           regenerate them
repository.yaml           makes this repo an HA add-on repository (see below)
schematics/               KiCad schematic + PCB, and gps-logger.pdf
```

## Hardware

- **Seeed Studio XIAO ESP32-C6** (PlatformIO env is named `esp32-c6-devkitm-1`
  for historical reasons, but `board =` inside it is the proper
  `seeed_xiao_esp32c6` definition, not the generic Espressif devkit one -
  the XIAO has its own onboard battery power management, so no separate
  charge IC is needed - see below)
- UART GPS module: **ATGM336H** at 9600 baud; its main VCC is switched by
  an AO3401 P-MOSFET (Q1, 150Ω gate resistor + 10kΩ gate pull-up) on
  D5/GPIO23, so the firmware cuts its power completely before sleep - see
  Wiring notes below
- ADXL345 accelerometer breakout (I2C) - used for motion-interrupt wake
  and the stillness filter (see Firmware behaviour)
- Active 3.3V buzzer for the low-battery and storage-almost-full alerts
- 3.7V LiPo battery, leads soldered directly to the XIAO's onboard
  BAT+ / BAT- pads
- Qi wireless charging receiver module (5V output) feeding the XIAO's 5V
  pin in parallel with USB-C - the onboard charge management handles the
  rest, so you can drop the tracker on a charging pad instead of plugging
  in a cable
- Voltage divider from battery+ to an ADC pin for battery monitoring
- A second small voltage divider off the XIAO's 5V pin, into a spare GPIO,
  to detect "charging power present" (see Charge status below)

Default pin mapping (override via `platformio.ini` `build_flags`). The
XIAO ESP32-C6 only exposes 11 GPIOs on its header, labeled `D0`-`D10` -
every assignment below comes from that set:

| Signal                    | XIAO pin | GPIO |
|---------------------------|----------|------|
| ESP32 RX ← GPS TX         | D9       | 20   |
| ESP32 TX → GPS RX         | D10      | 18   |
| GPS power control (Q1 gate)      | D5 | 23 |
| Battery ADC                | D1/A1    | 1    |
| ADXL345 SDA                | D7       | 17   |
| ADXL345 SCL                | D8       | 19   |
| ADXL345 INT1                | D2       | 2    |
| Charge status (5V-sense)  | D0       | 0    |
| Buzzer                      | D4       | 22   |

### Wiring notes

- **ATGM336H GPS**: power control is via an AO3401 P-MOSFET (Q1) gating
  the module's main VCC, not the module's own pin 5 (ON/OFF) - Q1's
  Source→3.3V, Drain→GPS VCC, Gate→D5/GPIO23 through a 150Ω series
  resistor (R1), with a 10KΩ pull-up (R2) from the gate node to 3.3V so
  the switch defaults OFF if the GPIO floats (e.g. during boot before
  pinMode() runs). Because it's a P-channel FET, the gate must go LOW
  (relative to Source) to turn ON - inverted from what driving pin 5
  directly would need - hence `GPS_POWER_ON_LEVEL` is `LOW` in
  `config.h`. `GpsReader::powerOn()`/`powerOff()` just drive
  `GPS_POWER_PIN` to that level (and its inverse), waiting
  `GPS_POWER_STABILIZE_MS` after powerOn() before expecting valid NMEA
  sentences - worth re-checking that delay is long enough now that a
  wake means the module's own regulator and digital core are restarting
  from a full power-off, not exiting an internal standby state the way
  the ON/OFF pin would. The module's RTC/SRAM backup power comes from a
  separate pin (VBAT, pin 6, 1.5-3.6V) that this breakout backs with its
  own small battery, independently of the switched main VCC rail, so
  that domain should stay alive even with VCC fully cut and a wake should
  land in the hot-start bucket (<=1s) rather than cold-start (<=35s).
  **Measured, it doesn't**: `[GPS] First fix N s after power-on` shows
  169-335 s after every wake, and a duty cycle that cut VCC for a few
  seconds got cold starts too. Only gaps of about a second (a restart)
  are bridged, by the module's own capacitors. See TODO.
- **ADXL345**: SDA→D7/GPIO17, SCL→D8/GPIO19, INT1→D2/GPIO2, VCC→3.3V,
  GND→GND. Leave INT2 unconnected. SDA/SCL sit next to the GPS wiring
  (D9/D10) for short runs to both modules; INT1 has to stay on D2 instead
  of joining them, though - ESP32-C6 only supports deep-sleep GPIO wakeup
  on its LP_IO pins (GPIO0-7), and D7-D10 (GPIO17/19/20/18) aren't in that
  range, so putting INT1 there would silently break motion-wake at the
  first sleep cycle. If your breakout ties the address-select pin
  (SDO/ALT ADDRESS) high, the firmware finds it anyway - when nothing
  answers at `ADXL_I2C_ADDR` (default `0x53`, tied low) it tries `0x1D`
  and logs a hint; set `ADXL_I2C_ADDR` to `0x1D` in `config.h` to match.
- **Qi receiver**: the XIAO ESP32-C6's onboard power management chip
  charges the battery whenever its 5V pin is powered - normally via
  USB-C, but the pin also accepts external 5V directly. Wire the Qi
  receiver's regulated 5V output straight into the XIAO's 5V pin (most Qi
  receiver modules are designed as drop-in USB replacements, so no extra
  regulation is needed) and the onboard charger takes it from there; no
  separate charge IC required.
- **Charge status**: the XIAO doesn't break out a CHRG/STAT line, so
  "charging" is inferred instead from whether 5V is present at all - the
  board's 5V pin carries USB-C VBUS when powered and reads 0V on battery
  alone. Add a resistor divider from the 5V pin down to D0/GPIO0 - two
  ~100k resistors (~2.5V when powered). While awake the firmware samples
  this pin via the ADC (>1.5V = charging); the deep-sleep plug-in wake
  uses the pin's digital threshold, which 2.5V only just clears, but it
  wakes reliably in practice (including out of the Shut down state). The
  charging state is surfaced in the web UI and the device-status payload
  the device POSTs to the backend.
- **Buzzer**: an active 3.3V buzzer has its own built-in oscillator, so it
  just needs DC to sound - wire its +/signal lead directly to D4/GPIO22
  and GND to GND, no driver transistor needed.
- **WiFi antenna**: the XIAO ESP32-C6 has a dual-antenna design (onboard
  ceramic antenna + a U.FL connector for an external one), switched by an
  RF switch on GPIO3/GPIO14. Neither of those GPIOs is used anywhere in
  this project, so the board just uses its onboard antenna automatically
  (the default) - no firmware changes needed. If you ever want to switch
  to an external antenna for better range, see the "onboard vs. external
  antenna" section of Seeed's wiki; GPIO3/GPIO14 aren't exposed on the
  header at all (they're internal to the antenna switch circuit), so this
  doesn't conflict with any of the peripheral wiring above.
- **GPS antenna in the enclosure**: reception depends almost entirely on
  how the GPS antenna sits in the box. After moving into the final
  enclosure the satellites used in a fix dropped from 15-21 to 6-10 - time
  to first fix went from 3-5 to 7-18 minutes and the fix kept dropping
  out while driving; re-seating the antenna's snap-on (IPEX/U.FL)
  connector and re-routing its cable brought it back to 14-20. Keep the
  ceramic patch facing the sky, with nothing above it (battery, PCB,
  screws, metal or carbon parts), a few centimetres away from the XIAO's
  WiFi antenna and the charging circuit, and the cable without sharp bends
  or strain on the connector. Check it with the debug log: the heartbeat's
  `sats=` while driving should be 14 or more, and `[GPS] First fix N s
  after power-on` after a wake 2-5 minutes (cold start, see TODO).

## Firmware behaviour

- **Boot priority: WiFi/OTA before peripherals**: `setup()` brings up WiFi
  (or the captive portal) and the web server (status, downloads, and the
  OTA endpoint) before touching any peripheral hardware - GPS, the
  ADXL345, the buzzer. A missing, miswired, or slow-to-respond sensor can
  therefore never block network availability, since it's initialized only
  *after* the device is already reachable. The ADXL345 driver also bounds
  every I2C transaction with `Wire.setTimeOut(50)`, so even a genuinely
  hung bus (a known ESP32 I2C driver failure mode when SDA/SCL are
  floating or miswired) fails fast within 50ms instead of hanging the
  chip - `MotionSensor::begin()` just returns false and the device
  continues booting normally without motion-wake. This combination means
  you can always recover a device with a hardware problem over WiFi/OTA
  without needing physical/USB access.
- **Logging**: fixes are filtered (min 3 s interval, min 5 m movement) and
  pushed into a RAM ring buffer (~2000 points capacity).
- **Indoor-fix gate**: indoors the receiver keeps a "good" fix (5-9
  satellites, HDOP 1.5-3) that wanders up to 300 m around the house with
  *fake speeds of 1-2 m/s* - enough to defeat the stop/go filter below
  and, with the device being carried around, the accelerometer's
  stillness filter too (45 minutes of scribble on 2026-10-08). So below
  8 km/h (`GPS_TRUSTED_SPEED_MPS` - slow cycling still counts) a fix is
  only used with open-sky quality: at least 6 satellites used in the fix
  and HDOP ≤ 1.2 - HDOP is the real filter, indoor fixes never got below
  1.5
  (`GPS_STATIC_MIN_SATS`, `GPS_STATIC_MAX_HDOP`); faster, any good fix is.
  Untrusted fixes are not logged, don't feed the stop/go state and count
  as "parked" for the WiFi decision. Checked against the recorded tracks:
  5962 of 5988 driving points kept (the rest standing at traffic lights),
  all 50 indoor points of that evening rejected (none reached 8 km/h);
  the night-time strays that pass are 1-2-point trips, which the rule
  below deletes.
- **Noise trips are deleted**: a trip that ends with fewer than 5 points
  (`MIN_SESSION_POINTS`) - typically a night-time bump that got a fix, or
  the device carried around the house - is deleted instead of being kept
  as a 1-point "track" (`[TrackStore] Trip of N point(s) discarded as
  noise` in the log). A fix additionally
  has to clear a reception-quality gate (HDOP ≤ `GPS_MOVEMENT_MAX_HDOP`,
  ≥ `GPS_MOVEMENT_MIN_SATS` satellites used) before it's logged at all -
  without it, indoor multipath (e.g. right after being carried in from a
  car) can report a "valid" fix that chaotically jumps many meters between
  updates while the device sits still, polluting the GPX with fake
  displacement.
- **Flash persistence**: buffer is flushed to a per-session GPX file on
  LittleFS every 15 minutes, once on entering low battery (≤15%), before
  sleep, before every restart (including OTA and Reset WiFi), before each
  upload pass, when the RAM buffer is full, and when a session ends. The
  GPX file is kept valid after every flush (footer is rewritten each
  time), so a sudden power loss never corrupts it. Every flush is verified
  against the file's real size, chunk by chunk (200 points); whatever
  didn't make it to flash (flash full) stays in RAM and is retried on a
  later flush - only once the 2000-point buffer is full is its oldest
  point dropped (see Flash space management below).
- **Sessions**: one GPX file per "trip", stored under
  `/tracks/YYYY-MM-DD/YYYY-MM-DD_HH-MM-SS.gpx` (local start time)
  — this is what shows up in both the on-device and backend calendar views.
  A trip ends after 7 minutes without movement (`STATIONARY_TIMEOUT_MIN`):
  on battery the device then
  deep-sleeps, and on a charger it stays awake but closes the session and
  powers the GPS down the same way (GPS stays on if the ADXL isn't working,
  since only its motion interrupt would turn GPS back on) - so an overnight charge doesn't merge
  the next morning's drive into yesterday's file. The next movement starts
  a new file. Planned restarts (WiFi recheck, charger plug-in, OTA) do
  *not* end a trip: the running session continues in the same file. The
  session's date/filename is assigned lazily, on the first real GPS
  point, rather than eagerly at boot - the system clock isn't set until
  GPS delivers its first fix (or NTP answers, charging only - see below),
  so starting a session any earlier would create a spurious `1970-01-01`
  folder; points arriving before that are dropped. From GPS the clock is
  only set from a sentence that carries both date and time (RMC, status
  `A`), with a plausible year.
- **Sleep (motion-interrupt driven)**: the ADXL345 is configured for
  hardware activity detection (375 mg threshold) on its INT1 pin, which is
  wired to a GPIO. While awake, every activity pulse resets an inactivity
  timer; if no motion has been detected for 7 minutes, the device flushes,
  cuts power to the GPS module via its hardware switch (D5/GPIO23), also
  releases the GPS UART and I2C (accelerometer) peripherals and tri-states
  their pins (`GpsReader::end()`/`MotionSensor::end()` - INPUT, no pull;
  the ADXL's own interrupt pin is left alone since it's the wake source
  itself) so nothing keeps driving current into an now-unpowered peripheral
  through its input protection diodes, and deep-sleeps. Before sleeping it
  checks once more for motion that happened during the pre-sleep flush and
  sync - if the device was just picked up, it stays awake. During sleep
  the ADXL samples at 25 Hz instead of 100 Hz (~60 µA instead of ~140 µA);
  in the long critical-battery sleep (battery still critical at boot -
  see Battery below), where motion wake is off, it is put in standby. The
  first sleep on dropping to ≤5% is still an ordinary one (2-hourly timer,
  motion wake armed, 25 Hz), just without the pre-sleep motion check.
  Both peripherals come back up on the next wake via their normal begin()
  calls in setup() - GPS only on a wake that needs it (see below) - since
  deep sleep re-runs the whole boot from scratch. The inactivity threshold (312.5 mg) sits one
  step below the activity threshold, so vibration can't block the
  sensor's activity detection from re-arming.
  Unlike a periodic GPS-coordinate check, the device does
  **not** wake on a fixed schedule to take a GPS fix just to see if it
  moved - it stays asleep until the ADXL345 itself interrupts the ESP32 on
  real motion (µA-level sensor, vs. powering up GPS every minute), which is
  a substantial power saving. The ADXL isn't the only thing that resets
  the inactivity timer while awake, though: a logged GPS point also counts
  as activity, independent of the accelerometer. Smooth highway cruising
  can stay under the ADXL's activity threshold for longer than the
  7-minute timeout, and without this the device would fall asleep
  mid-drive. This vote is gated more strictly than logging itself, though
  (`GpsReader::isConfirmedMoving()`, not just `hasGoodFix()`): HDOP and
  satellite count reflect satellite geometry, not multipath, so a fix
  indoors can pass the logging quality gate - decent HDOP, enough
  satellites - while its position still jitters many meters between
  updates, on a budget single-constellation receiver like the ATGM336H on
  this board. What's actually relied on to reject that jitter is
  two speeds that must BOTH clear `GPS_MOVEMENT_MIN_SPEED_MPS` (default
  0.5 m/s): the **position-derived speed** (distance to the last logged
  point divided by the time since that point was logged - measured by the
  clock, so it stays correct across restarts and wakes) and the receiver's
  own **Doppler speed over ground**, which multipath jitter barely affects.
  Only a fix clearing both counts as "moving" for the sleep decision; one
  that's merely good enough to log can still fail to keep the device awake.
- **Stillness filter (indoor jitter)**: indoors a receiver lying still
  keeps producing "good" fixes (HDOP ~1, 5-14 satellites) that jump 5-10 m
  with a fake 0.4-1.9 m/s speed - the scribble at the end of a trip after
  arriving home. HDOP, satellite count and reported speed can't separate
  that from real driving, but the ADXL knows the device is lying still:
  while it reports inactivity (3 s below its inactivity threshold, and no
  activity since) and the GPS speed is below 2 m/s
  (`GPS_STILL_MAX_SPEED_MPS`), no point is logged. Driving is still logged
  on speed alone (smooth highway cruising looks "still" to the ADXL), and
  walking with the device never looks still (no 3 s of quiet). Only a slow
  crawl below 7 km/h without any vibration loses points - e.g. waiting at
  a red light, which is desirable anyway.
- **Stop/go filter (carried indoors)**: the ADXL filter can't help when
  the device is carried around the house after a trip - it is moving, but
  the GPS fix indoors drifts 5-100 m with a Doppler speed of ~0. So the
  receiver's own verdict is used as well: after 3 consecutive fixes below
  0.5 m/s (`GPS_STOP_MAX_SPEED_MPS`, `GPS_STOP_FIXES`) the receiver says
  "stopped", and until 2 consecutive fixes report at least 1 m/s again
  (`GPS_GO_MIN_SPEED_MPS`, `GPS_GO_FIXES`) a point is only logged when it
  is 50 m (`TRACKPOINT_STOPPED_MIN_DISTANCE_M`) from the last one instead
  of 5 m - and that far position has to hold for 3 consecutive good fixes
  (`GPS_STOPPED_FAR_FIXES`), so a lone drift spike 100 m away is not a
  move either. On a real track this turned a 54-point scribble around the house
  into 3 points and left the drive untouched; waiting at a red light gets
  quieter too. The cost: a very slow stroll (under 3.6 km/h by the
  receiver's speed) is thinned to one point per 50 m.
- **GPS duty cycle while tracking (battery) - implemented, DISABLED**:
  the ATGM336H draws ~25 mA whatever its fix rate (radio and tracking
  hardware run continuously, and 1 Hz is the slowest rate it offers), so
  the idea is to switch it *off between fixes* and rely on the backup
  cell on VBAT for a ~1-s hot start. **On this board that does not work:**
  the first two drives with it (2026-10-05/06) got a fix only minutes
  after each power-on - 7-8 points per 40-minute drive, mostly at speed
  0, the signature of cold starts. The backup cell evidently doesn't hold
  the receiver's state (it is charged from VCC, which is off most of the
  time); only power gaps of about a second (a restart) are bridged, by the
  module's own capacitors. So `GPS_DUTY_PERIOD_MS` is 0. The
  `[GPS] First fix N s after power-on` log line shows when a fresh backup
  cell (or keeping the module in standby via its ON/OFF pin instead of
  cutting VCC) makes it viable. As implemented:
  moving: one cycle every 5 s (`GPS_DUTY_PERIOD_MS`) - on until 2 good
  fixes (`GPS_DUTY_SETTLE_FIXES`; the first after power-on is the least
  accurate and is not logged), the second is logged under the usual
  rules, then off for the rest of the period (at least 1.5 s). Stopped
  (the receiver says stopped, or the ADXL says still and the speed is
  below 2 m/s): off for 30 s (`GPS_DUTY_STOPPED_OFF_MS`), ended early by
  accelerometer activity. Without a good fix the module just stays on
  (then the no-fix duty cycle below applies), and on a charger it tracks
  continuously at 1 Hz as before. With hot starts that would be a point
  every ~5 s while driving at roughly half the GPS current.
- **Light sleep while the GPS is off (battery) - implemented, DISABLED**:
  `LIGHT_SLEEP_ENABLED` is 0 because after a light sleep the ADC read the
  charge-detect pin and the battery voltage too high (battery +12-15 %,
  "charging" while on battery), which caused 27 "charger appeared"
  restarts in 30 minutes of driving on 2026-10-06. It needs a bench
  session on USB serial first (probably re-initialising the two analog
  pins after each wake). As implemented: in those off periods
  (and in the no-fix off periods), with WiFi off and no beep running, the
  CPU light-sleeps in 1-s slices (`LIGHT_SLEEP_SLICE_MS`) instead of
  idling at ~20 mA; each slice end looks at the accelerometer's latched
  activity flag, the battery and the timers. With the GPS off there is no
  UART traffic that light sleep could garble. The heartbeat's `gps_on=`
  and `lsleep=` show the share of each minute the module was powered and
  the CPU slept.
- **GPS duty cycle without a fix (battery)**: a device carried around
  indoors keeps the inactivity clock alive while the receiver searches in
  vain at ~25 mA - one evening of that drained 25 %. After 10 minutes
  without a fix (`GPS_NOFIX_SUSPEND_AFTER_MS`) the module is powered off
  for 5 minutes (`GPS_NOFIX_OFF_MS`), then given 2 minutes
  (`GPS_NOFIX_RETRY_ON_MS`) to find one, and so on until it does (outdoors
  a cold start takes ~35 s). Plugging in a charger ends the suspension.
  A tunnel or garage long enough to trigger it costs at most one off
  period of track.
- **Shut down (web UI)**: **Settings → Power → Shut down** closes the
  trip like the stationary timeout does, powers the GPS down, puts the
  accelerometer into standby (measurement off, ~0.1 µA) and deep-sleeps
  with **only the charger wake armed** - no motion wake, no timer wake.
  The device is then off for all practical purposes (a few µA: the C6's
  deep sleep, the LDO and the battery divider) until a charger is
  connected, which wakes it like any plug-in does and runs a normal boot.
  Refused while charging (it would wake again within a second - unplug
  first) and while a sync pass is running.
- **GPS is gated by motion**: it's only powered on and tracked on a fresh
  boot or a wake actually caused by the ADXL's motion interrupt (a planned
  restart keeps the GPS state it had) - a safety-net timer wake (default
  every 2 h, `SAFETY_WAKE_INTERVAL_SEC`) with no motion detected skips GPS
  entirely and is purely a battery-level check before going straight back
  to sleep, and a charger plug-in wake also starts with GPS off. Real ADXL
  activity later in such a cycle (picked up right after a timer wake,
  driven off while charging) powers GPS on at once. Exception: if the
  ADXL failed to initialise, or the motion wake couldn't be armed for the
  last sleep (INT1 stuck LOW, arming error), the 2-hourly timer wake is
  treated like a motion wake - except after a critical-battery sleep. The
  GPS UART has an 8 KB receive buffer, so a sync pass blocking the loop
  for seconds doesn't lose fixes.
- **WiFi comes up when the movement stops, not while moving**: WiFi and
  the web server run in bounded windows - forced off again after
  `WIFI_ACTIVE_WINDOW_MS` (5 minutes by default), since a window is a
  sync/OTA opportunity rather than something that needs to stay on, and
  WiFi is one of the biggest power draws on this chip. A fresh boot opens
  one; **a motion wake does not** (the device is about to move, and a
  bump that never becomes a trip then costs no WiFi at all); a
  safety-timer wake never does. Instead, once the GPS receiver has
  reported "stopped" - or has no trustworthy fix, i.e. indoors (see the
  indoor-fix gate) - for `WIFI_RECHECK_STILL_MS` (45 s), the device
  flushes and restarts into a fresh window. **After a drive** (a point
  logged at 18 km/h or more since the last window,
  `WIFI_MOVED_MIN_SPEED_MPS`) that happens at the first such stop, so
  arriving at home or the office syncs within about a minute. That
  **arrival window stays up until the device goes to sleep** (7 minutes
  after the last movement, capped at 10 minutes, `WIFI_ARRIVAL_WINDOW_MS`)
  instead of closing after 90 s with nothing to do, so the web UI is
  reachable while you walk in (~1-2 % battery per arrival). Such
  stop-triggered windows are at least 3 minutes apart
  (`WIFI_STOP_WINDOW_MIN_GAP_S`), so stop-and-go traffic can't restart the
  device every minute. "Stopped" means either **2 minutes** standing
  still outdoors - the receiver says stopped, with a good fix
  (`WIFI_STOPPED_OUTDOOR_MS`; a red light or a junction is not an
  arrival) - or, indoors, 45 s without a trustworthy fix *while the
  accelerometer is quiet too*. A fix lost at driving speed (tunnel, poor
  reception) counts only after 5 minutes (`WIFI_LOST_FIX_PARKED_MS`): on
  2026-10-12/13 a missing fix while driving restarted the device six
  times mid-drive. Without a drive (handled at home, walking) the recheck interval
  applies: at the earliest `WIFI_RECHECK_INTERVAL_MS` (5 minutes) after
  the last window, never for an unconfirmed bump, and after a motion wake
  not before the GPS has had a trustworthy fix (or 10 minutes,
  `WIFI_NOFIX_FIRST_RECHECK_MS`) - until then the device can't tell
  whether it is about to drive off.
  No association completes at driving speed anyway, and each window is a
  restart plus up to a minute of scanning at ~100 mA - seven of them on a
  42-minute commute once drew more than the GPS did. And when a window found a backend with nothing pending and
  no point has been logged since boot, the next recheck waits 30 minutes
  (`WIFI_RECHECK_IDLE_INTERVAL_MS`) instead of 5 - the device is parked
  somewhere with nothing to say; the first newly logged point pulls the
  recheck back to the 5-minute cadence. A connected window on battery
  that found nothing to upload and has served no web request for 90 s
  (`WIFI_IDLE_CLOSE_MS`) is closed early as well - except the arrival
  window - and a page left open keeps it alive through its status polls. The CPU runs at 80 MHz throughout
  (`CPU_MHZ_AWAKE`): enough for GPS parsing, flash, WiFi and the web
  server, at roughly two thirds of the core current of 160 MHz. So in practice **the
  device is reachable over WiFi/OTA whenever it is parked for a while**
  (or via the captive portal when WiFi isn't configured yet), not while
  rolling. The exception is charging: WiFi also activates whenever
  the charger is present, independent of motion, and neither the active
  window's cutoff nor the periodic recheck apply while charging - deep
  sleep is already skipped entirely then (see Battery below), so WiFi just
  stays on continuously, which is also what lets NTP sync (see below) work
  at all while sitting stationary on a charging pad. A recheck restart
  only happens after recent motion (within the last recheck interval) and
  a confirmed wake, not after a single bump. There's no
  physical setup button in this design - the WiFi/backend setup portal
  opens automatically when no WiFi network is saved (at power-up, or on
  the charger once the device has been still for 10 minutes - then
  re-offered every 10 minutes; never on battery motion wakes or while
  driving on a car charger, where it would block GPS for 3 minutes), after
  **Reset WiFi** in Settings, or once when the device sits still on the
  charger unable to reach either saved network (see "To change WiFi later"
  in Part 4 above). Charger plug/unplug reactions are debounced by 5 s, so
  a flaky Qi pad or car USB doesn't trigger restarts.
- **Battery**: on entering low battery (≤15%) the buffer is force-flushed
  once, and a buzzer sounds 5 short beeps, repeating once a minute for as
  long as the battery stays low (it stops immediately if you plug in/place
  the device on the Qi pad and charging is detected, or once the battery is
  critical - see below). At ≤5% on battery the session is closed and the
  device sleeps; while charging, a critical reading changes nothing. If the
  battery is *already* critical at boot (e.g. a 2-hourly safety wake finds it
  still critical) and not charging, the device skips WiFi, GPS, and every
  other peripheral entirely (the ADXL goes to standby) and goes straight
  back to sleep for a much longer interval
  (`CRITICAL_BATTERY_SLEEP_INTERVAL_SEC`, 6h by default) instead of the
  normal 2-hourly cadence - protecting the cell from further drain takes
  priority over remote reachability in this state, so the device will
  *not* be reachable over WiFi/OTA while critical and unplugged. After a
  critical sleep it only resumes normal operation once the battery reads
  at least 3 percentage points above the critical level
  (`CRITICAL_RESUME_MARGIN_PERCENT`) - a cell reads higher at rest than
  under load, and without that margin it would cycle between waking and
  sleeping. The serial port only waits for a USB host (up to 4 s) when USB
  power is present, not on battery wakes.
- **Deep sleep is battery-only**: whenever the charger (Qi pad or USB-C) is
  present, deep sleep is skipped entirely, at every trigger point -
  inactivity timeout, low/critical battery, all of it. There's no reason
  to conserve power while actively being charged, and staying awake means
  continuous WiFi/OTA/GPS availability while it's sitting on the pad.
  Normal motion-driven sleep resumes automatically once unplugged.
- **NTP time sync (charging-only)**: GPS is the primary time source (see
  "Sleep" above - it syncs the clock from NMEA data whenever it gets a
  fix) and that's sufficient out in the field. Indoors on a charging pad,
  though, GPS frequently can't get a fix at all - so whenever the device
  is both charging and connected to WiFi, it also starts the ESP32's
  built-in SNTP client as a supplemental time source, once per wake cycle.
  This never runs on battery, so it costs nothing in the field where GPS
  already handles it.
- **Clock drift in deep sleep**: the XIAO has no 32 kHz crystal, so time
  through deep sleep is kept by the ESP32-C6's internal RC slow clock,
  calibrated once at boot and temperature dependent - observed 0.3-0.5 %
  (the device runs fast: -224 s over a 12-hour night; 0.6-1.1 % over short
  evening sleeps). It doesn't reach the tracks (a point is only logged
  after GPS has set the clock) or the lifecycle (the inactivity clock is
  re-based on steps); it does affect the minutes after a motion wake until
  the first fix, and shows up as a backwards time step in the debug log.
  The first GPS sync of each boot logs the error it corrected, over the
  time since the clock was last set exactly by GPS or NTP (`[Time] First
  GPS sync: clock was -224 s off (GPS minus device) 11.9 h after the last
  clock sync = -0.523 % (device fast)`), so the drift can be judged over a
  few nights before deciding on compensation.
- **Status reporting**: battery %, charging state, RAM buffer usage
  (points used / free out of the ~2000-point capacity), GPS fix state and
  satellite count, and the board's current synced time are exposed both on
  the on-device web UI (`/api/status`) and POSTed periodically to the
  backend (`POST /api/v1/devices/<id>/status`, whenever the device is awake
  with WiFi), where the backend UI shows it as a live device-status line. The board's clock is only
  synced from GPS NMEA data (no RTC chip), so `timeSynced` is `false` and
  `boardTime` is `null` until the first fix ever arrives - avoids showing a
  confusing 1970 date before then (the same issue that could otherwise
  affect session folder names - see Sessions above).
- **WiFi (two networks)**: the device stores up to two WiFi networks - a
  primary and a second one, e.g. home and office - in `config.json`, and
  joins whichever is in range. It tries the network that connected last
  first (remembered across deep sleep, so a device living at one site
  doesn't pay a timeout per wake for the other), then alternates: each
  attempt gets 20 s (`WIFI_SLOT_TRY_MS`), or ~6 s once the driver has
  already given up on the network (not in range, or a wrong password - the
  latter is recognised from the disconnect reason). Once both networks have
  been reported "not found", it scans only every 30 s, and on battery the
  WiFi window closes after 45 s of that instead of running for 5 minutes -
  scanning constantly on drives without known WiFi was a large part of the
  battery drain. Both networks are
  edited in **Settings → WiFi**; passwords are never sent back to the
  browser (leave a field empty to keep the saved password). The primary
  network is first set in the `WiFiManager` captive portal AP
  (`GPSLogger-Setup-XXXX`, together with the backend host/port); devices
  set up before the two-network change import their saved network as the
  primary automatically on first boot. In the portal, a failed connect
  (e.g. a typo in the password) keeps the portal open for another try, and
  a network is only saved once the device has actually connected to it -
  a portal that times out changes nothing. The portal's firmware upload
  and config erase pages are disabled (not just hidden - the AP is open,
  so anyone in range could otherwise flash firmware through it). While
  charging, if no saved network is reachable for 3
  minutes, the device first reboots into the setup portal once (only if it
  has also been still for 10 minutes - a red light or traffic jam on a car
  charger doesn't count), then restarts once into a normal boot to recover
  a wedged radio, and after that just keeps alternating in place.
- **Sync (two backends)**: up to two backends (e.g. home and office Home
  Assistant) are configured in **Settings → Backend**. At sync time the
  device probes them in order (`GET /healthz`, 5 s connect timeout) and
  syncs to the first that answers - home and office can even share the
  same IP range, since the probe, not the network, decides; the choice is
  re-made whenever WiFi (re)connects, e.g. after moving to the other site.
  When neither answers, the next probe follows after 15 s rather than the
  regular 30 s, and the pause after ten failed rounds is 2 min (not the
  10 min of the upload give-up): on arrival at a site the LAN path from a
  freshly associated client to the backend has been seen to take 2-10 min
  to come alive while the WiFi link itself was fine. The
  record of which sessions were uploaded is kept **per backend and with
  the file size** the backend received: the session being recorded is
  uploaded whenever a WiFi window allows, and uploaded again each time it
  has grown, so the backend always ends up with the complete trip. Whenever
  WiFi is connected, the device checks for unsynced sessions every 30 s
  (and immediately on wake, and before sleep), PUTs them to the backend's
  REST API, and POSTs its live status. After 10 consecutive connection
  failures sync pauses for 10 minutes (`SYNC_GIVEUP_RETRY_MS`; also lifted
  by **Sync now** or a WiFi reconnect) - it no longer waits for a wake that
  never comes on a charger. A file that backend *rejects* (HTTP 4xx other
  than 408/429), or that an answering server keeps failing on 3 times in a
  row, is skipped without blocking the others and is retried after 30
  minutes, when it changes, on **Sync now**, or when WiFi reconnects; the
  status line shows how many files are skipped. Connection-type failures
  (no answer, timeouts, 502/503/504 while a backend restarts) never park a
  file - they make the next check re-probe both backends, so the device
  fails over to the other one. **Sync now** pressed while a pass is running
  takes effect when the pass ends. Empty session files (left behind by a reset in the middle of a
  write) are never uploaded and are removed. The upload record lives in RAM
  and is saved atomically (temp file + rename), so a reset can't corrupt it.
- **Flash space management** (**Settings → Storage**): the device keeps
  every track it has room for, including ones already uploaded. Only as a
  last resort - when the next track flush would not fit into the 896 KB
  storage otherwise while still leaving a 64 KB margin free *after* the
  flush (`SYNC_FS_MIN_FREE_BYTES`; a margin measured before the flush let
  the debug log go silent mid-drive) - it
  deletes its **oldest tracks whose current version is stored on a
  backend**, with or without WiFi, so a long offline trip can still use
  that space. Never the track being recorded (nor its date folder), never
  unsynced data. The switch **Delete uploaded tracks when space runs out**
  (on by default) turns this off completely: then nothing is ever deleted
  automatically, and when the flash is full, recording stops until you
  delete tracks in the web UI. **Tracks that aren't uploaded yet are never
  deleted automatically** - so the device warns before they fill the
  flash: when free space plus the removable uploaded tracks drops below
  160 KB (`STORAGE_WARN_BYTES`, about an hour of driving), a **red banner**
  appears at the top of the web UI (critical below 64 KB), and the
  **beeper** sounds 3 long beeps every 30 minutes while the device is
  awake - only in daytime, 08:00-21:00 local time (and only once the clock
  is set), so it never beeps at night. It's distinct from the low-battery
  alert (5 short beeps every minute), which takes precedence. The warning
  clears by itself once the tracks are uploaded (or deleted). Flushes are
  written in committed chunks of 200 points, so a nearly full flash still
  stores what fits. If the flash is full anyway (a very long trip with
  nothing reclaimable), new points stay in RAM (up to 2000, ~1.7 h at one
  point per 3 s) instead of being silently dropped, and a failed flush is
  retried at most once a minute. While such unsaved points are held, the
  optional restarts that WiFi doesn't depend on (recovery, portal offer)
  wait up to 30 minutes for the flush to succeed; the restarts that bring
  WiFi back (recheck, charger plug-in) go ahead, since only a sync can free
  a flash full of unsynced tracks. Stale debug-log files are
  deleted before any track data when space runs short. Trade-off with two backends: a session
  reclaimed after reaching one backend never reaches the other; this only
  happens under space pressure. Flash usage is shown on the main page.
- **OTA**: **Settings → Firmware update (OTA)** - pick a `firmware.bin`
  and hit Flash; a progress bar tracks the upload (which writes to flash
  synchronously as it streams in), followed by a verification check
  against the device and a countdown while it reboots. Under the hood it's
  `POST /update` with a multipart firmware binary (standard ESP32 `Update`
  library flow), so it can still be scripted directly:
  `curl -F "firmware=@.pio/build/esp32-c6-devkitm-1/firmware.bin" http://<device-ip>/update`.
  It answers `OK, rebooting` (HTTP 200) only after a complete, verified
  flash; a corrupt image, or an upload started while a sync pass is
  running, is refused with an error message (HTTP 500) and the device
  keeps running its current firmware. Before rebooting into the new
  firmware it saves the RAM track buffer, and the running trip continues
  after the update. Remember the device is only
  reachable during its bounded WiFi window (movement, or charging - see
  "Sleep" above), so have it moving or on the charger when you want to
  push an update.
- **On-device web UI**: `http://<device-ip>/` shows the everyday view -
  battery/WiFi (with the connected network)/sync status, active backend,
  flash usage, board time (`Date: dd/mm/yyyy | Time: hh:mm:ss`, local time
  with a DST indicator), GPS fix and a satellite table, and a **month
  calendar** of tracks like the backend's: days with tracks are
  highlighted with a session count, click a day to list its sessions with
  download and delete links (per session, or "delete date" for a whole
  day). The **Settings** page (`⚙ Settings`, or `#settings` - bookmarkable,
  browser Back works) holds configuration: both backends, both WiFi
  networks, Reset WiFi, timezone, storage, the debug log, a **Shut down**
  button (see Firmware behaviour) and the OTA firmware update. The theme follows the time of day automatically: light from
  07:00 to 19:00, dark otherwise, judged by the clock of the phone/laptop
  viewing the page (re-checked every minute, so an open tab switches by
  itself in the evening). There's no
  map on the device - the map library would not fit its flash; use the
  backend UI for maps. There's no login either (LAN device), but no other
  *website* can drive it: state-changing requests carrying an `Origin`/
  `Referer` of a different host are refused (so a malicious page open in a
  browser on the LAN can't flash firmware or redirect the backend), and
  requests addressed to a non-local hostname are refused (DNS rebinding).
  Accepted host names: IP addresses, single-label names, and the suffixes
  `.local .lan .home .home.arpa .internal .localdomain .fritz.box
  .speedport.ip .mshome.net .ts.net` - add your own (e.g. an office DNS
  zone) with the build flag `-D WEB_EXTRA_HOST_SUFFIX=\".corp.example.com\"`.
  Scripts like `curl` from the LAN still work. The download/delete
  endpoints only accept plain file and date names, so nothing outside the
  track folders (e.g. `config.json` with the WiFi passwords) can be
  fetched.
- **Debug log (switchable, in the web UI)**: **Settings → Debug log →
  Record debug log** turns on recording of everything the firmware logs,
  with timestamps (`dd.mm HH:MM:SS` once the clock is set, `+uptime`
  before), plus extra detail only printed in this mode: backend probe
  results with HTTP codes, each sync check and pass result, status-post
  results, WiFi disconnect reason codes, flash usage at each space check,
  sleep decisions, and an extended heartbeat (free heap, RSSI, flash,
  pending sessions, satellites). Every boot records the **reset reason**
  (power-on, software restart, deep-sleep wake, PANIC, WATCHDOG, BROWNOUT,
  POWER GLITCH, CPU LOCKUP) - usually the only trace of why a device
  dropped off the network. The log is saved to flash every 30 s, before
  deep sleep and before every restart, so it survives reboots, sleep and
  power loss (up to the last 30 s before a crash or power loss can be
  lost). Chatty events are summarised once a minute with a count instead
  of logged individually (ADXL still/moving, accelerometer activity
  interrupts, GPS stop/go) - the raw activity lines alone would otherwise
  fill the log in under an hour of driving. It rotates at 48 KB, so at
  most ~100 KB is ever stored; below
  32 KB of free flash (`DEBUG_LOG_MIN_FREE_BYTES`) it stops writing and
  gives back its older half, so it never crowds out tracks. That limit is
  deliberately below the 64 KB the track cleanup keeps free - at the same
  value the log regularly lost its history. Boots that go straight back to sleep because
  the battery is critical aren't recorded (that path deliberately does
  nothing but sleep). **Show log** displays it in the page;
  **download** saves it as `gpslogger-debug.log` (`GET /api/log`), and
  **clear** deletes it. The setting itself is kept across reboots. Switch
  it off when you're not diagnosing a problem, since it adds flash writes.
  Serial output is unchanged either way.
- **Heartbeat**: a one-line `[Heartbeat]` is printed every minute (uptime,
  WiFi state and IP, charging, battery, GPS, inactivity). A healthy idle
  device otherwise prints nothing for minutes, which looks exactly like a
  hung one - with the heartbeat, more than a minute of silence means the
  firmware really is stuck.
- **Timezone**: set in **Settings → Timezone** from a dropdown of common
  zones (or a custom POSIX TZ string, e.g. `CET-1CEST,M3.5.0,M10.5.0/3`;
  an active custom zone is shown with an **edit** link). This uses the C
  library's own DST engine - the string encodes both the base UTC offset
  and the DST transition rule, so summer/winter time switches automatically
  on the right dates with no manual twice-a-year toggling. It affects the
  web UI's displayed board time, the device-status payload, and session
  date-folders/filenames (so trips are grouped and named by local calendar
  day, not UTC day - a trip starting at 11pm local wouldn't otherwise get
  filed under "tomorrow"). It never touches the actual `<trkpt><time>`
  values inside GPX files, though - those stay UTC always, per the GPX
  spec, for correct interchange with other tools.

## Power consumption

Where the battery goes, per operating mode, and what the firmware does
about it. Currents are estimates from datasheet-level figures and the
debug log's battery readings, not bench measurements - the heartbeat's
`gps_on=` share shows how long the GPS module was powered.

| Mode | Average draw (est.) | Dominant consumers | Measures |
|---|---|---|---|
| Shut down (web UI) | a few µA | C6 deep sleep, LDO/charger IC quiescent, battery divider | ADXL in standby, no motion or timer wake - only a charger wakes it |
| Deep sleep (between trips) | ~60-120 µA | ADXL at 25 Hz (~60 µA), divider, LDO | ADXL at 25 Hz instead of 100 Hz; timer wake only every 2 h (`SAFETY_WAKE_INTERVAL_SEC`), GPS/WiFi stay off on those wakes |
| Tracking on battery | ~45-50 mA (was ~75-80 mA) | GPS ~25 mA, CPU ~20 mA | CPU at 80 MHz; no WiFi rechecks while rolling (only once parked); the GPS duty cycle and light sleep are implemented but disabled (see Firmware behaviour) |
| Carried indoors, no fix | ~30 mA after 10 min (was ~55 mA) | CPU, GPS searching | No-fix duty cycle: GPS off 5 min / on 2 min after 10 min without a fix |
| Parked, WiFi window | ~40-100 mA, for ≤90 s - 10 min | WiFi radio | No window on a motion wake or while moving; one once stopped (2 min outdoors with a fix, 45 s indoors with a quiet accelerometer), right away after a drive; the arrival window stays up until the device sleeps (max 10 min), others close after 90 s with nothing to upload and no web request; any window closes after 45 s if no known network is in range; next recheck after 30 min when idle |
| Charging | irrelevant | - | Continuous 1 Hz GPS, WiFi on, no sleep |

The biggest single cost used to be **WiFi rechecks while driving**
(seven 1-minute scan windows on a 42-minute commute - more than the GPS
itself), followed by the **GPS searching indoors** while the device was
carried around, which kept the inactivity clock alive for hours. Both are
gone; on a commute the GPS module and the awake CPU now share the bill
about equally.

Remaining levers, in order of payoff:
- A working GPS hot start - a fresh backup cell on VBAT, or wiring the
  module's ON/OFF pin so it can sit in standby instead of losing VCC -
  would make the implemented duty cycle usable (roughly half the GPS
  current). Check `[GPS] First fix N s after power-on` after any change.
- Light sleep, once the ADC-after-wake problem is solved on the bench
  (the CPU idles at ~20 mA between NMEA sentences).
- Higher-value battery and charge-detect dividers (the leak through them
  is part of the deep-sleep floor - keep the ADC input impedance in mind).
- ADXL supply decoupling (see TODO) would allow a lower activity
  threshold, not lower current - listed here only because a quieter
  sensor also means fewer false activity wakes.

## Building the firmware

```bash
cd firmware
pio run                      # build
pio run -t upload            # flash over USB
pio device monitor           # serial log
```

First boot (or after a factory reset) will bring up the `GPSLogger-Setup-*`
WiFi AP — connect to it, and the captive portal lets you pick your WiFi
network and enter the backend IP/hostname and REST port.

## Backend deployment

There's a single `backend/` folder for both deployment paths - it builds
offline from the vendored `backend/wheels/` either way (see the Dockerfile
comments), so there's no separate add-on copy to keep in sync anymore.

### Option A — plain Docker / docker-compose on the Pi

```bash
cd backend
docker compose up -d --build
```

This starts the FastAPI backend on port 8080.

Tracks are stored under `backend/data/tracks/<deviceId>/<date>/<session>.gpx`.

### Option B0 — one-click install from a git host (easiest, once published)

This repository doubles as a Home Assistant **add-on repository**
(`repository.yaml` at the root + the add-on in `backend/`). Once it is
pushed to a public git host, anyone can install the add-on without copying
files around:

[![Add repository to my Home Assistant](https://my.home-assistant.io/badges/supervisor_add_addon_repository.svg)](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2Fmakserge%2Fgps-logger)

Clicking the badge opens your own HA instance and pre-fills the
"Add repository" dialog with `https://github.com/makserge/gps-logger` -
one confirmation, and "Offline GPS Logger" appears in the add-on store,
with the normal update flow on every new `version:` pushed to this repo.

### Option B — Home Assistant Custom App

Copy the whole `backend/` folder into your Home Assistant local `/addons/` directory.

1. Go to **Settings** > **Apps**.
2. Click **Install App** in the bottom right corner to enter the store.
3. Click the three dots (`⋮`) in the top-right corner and select **Reload**.
4. Supervisor finds `config.yaml` and places "Offline GPS Logger" at the very top under **Local Apps**.
5. Select it, click **Install**, and wait for the hardware compilation step to finish.

It stores tracks under HA's `/share/gpslogger/tracks` and exposes its web UI via HA's Ingress (shows up in the sidebar) as well as directly on port 8080 for the ESP32 devices to reach. There are no options to configure.

**Note:** the vendored `backend/wheels/` currently only cover aarch64
(matching a Raspberry Pi / HA-on-aarch64 host). `config.yaml` lists `amd64`
as a supported arch too, but the build will actually fail there until you
regenerate the wheels for amd64 with `backend/wheels/update-wheels.sh`
(needs internet - the Docker/Supervisor build itself does not have it).

## Compatibility with existing tracking tools

Sessions are stored as standard **GPX 1.1** files with `<trkpt>` elements
(lat/lon/ele/time plus `speed`/`course`/`battery` extensions), so they open
directly in GPX Studio, GPSBabel, Garmin BaseCamp, OsmAnd, Google Earth,
Strava/Komoot import, QGIS, etc. without any conversion step.

## REST API summary (backend)

| Method | Path | Purpose |
|---|---|---|
| PUT | `/api/v1/tracks/{deviceId}/{date}/{filename}` | Device uploads a GPX session |
| GET | `/api/v1/devices` | List known devices |
| GET | `/api/v1/devices/{id}/dates` | List calendar dates with sessions |
| GET | `/api/v1/devices/{id}/dates/{date}/sessions` | List sessions for a date |
| GET | `/api/v1/devices/{id}/dates/{date}/sessions/{file}` | Download a GPX file |
| GET | `/api/v1/devices/{id}/dates/{date}/sessions/{file}/points` | Track geometry for the map view |
| POST | `/api/v1/devices/{id}/status` | Device posts its live status (battery, charging, GPS, board time...) |
| GET | `/api/v1/devices/{id}/status` | Last status received from the device |
| DELETE | `/api/v1/devices/{id}/dates/{date}/sessions/{file}` | Delete a track (empty date/device folders are removed too) |
| GET | `/tiles/{z}/{x}/{y}.png` | Cached map-tile proxy for the web UI |
| GET | `/healthz` | Liveness probe - the device uses it to pick which of its two backends is reachable |

Uploads must be parseable GPX. A file damaged by a reset in the middle of a
write (header or footer missing, a half-written trackpoint) is **repaired**
on upload: every complete `<trkpt>` is kept and the GPX envelope rebuilt
around them (logged as `Repaired damaged GPX ...`). Only a file with no
complete trackpoint at all is rejected (HTTP 400).

## REST API summary (device)

Served by the device itself at `http://<device-ip>/` (plain HTTP, no auth -
LAN only):

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/status` | Battery, charging, WiFi (networks, connected SSID, RSSI - never passwords), backends, sync progress, flash usage and storage warning level (`storage`), GPS, board time |
| GET | `/api/sessions` | `{date: [session files]}` for the calendar |
| GET | `/api/download?date=&file=` | Download one GPX session |
| DELETE | `/api/sessions?date=[&file=]` | Delete a session, or a whole date |
| GET | `/api/satellites` | Satellites in view with elevation/azimuth/SNR |
| POST | `/api/sync` | Start a sync pass now |
| POST | `/api/backend?host=&port=&host2=&port2=` | Set both backends (empty `host2` removes the second) |
| POST | `/api/wifi` (form body `ssid`, `pass`, `ssid2`, `pass2`) | Set both WiFi networks (empty password = keep saved; empty `ssid2` removes the second) |
| POST | `/api/timezone?tz=` | Set the POSIX TZ string |
| POST | `/api/reset-wifi` | Erase both WiFi networks and reboot into the setup portal |
| POST | `/api/shutdown` | Close the trip and deep-sleep with only the charger wake armed (409 while charging) |
| POST | `/update` (multipart `firmware`) | OTA firmware update |
| GET | `/api/ota/status` | Result of the last OTA attempt |
| POST | `/api/debug?on=1` / `?on=0` | Switch debug-log recording on/off (kept across reboots) |
| POST | `/api/storage?reclaim=1` / `?reclaim=0` | Allow/forbid deleting uploaded tracks when space runs out |
| GET | `/api/log[?download=1]` | The debug log as plain text, oldest first |
| POST | `/api/log/clear` | Delete the stored debug log |

Mutating endpoints answer `503 sync in progress` while a sync pass is
running (the web server is serviced between file uploads); retry a few
seconds later.

## Notes / things to tune for your hardware

- Battery divider constants (`BATTERY_ADC_MAX_MV`/`MIN_MV`,
  `BATTERY_DIVIDER_RATIO`) in `firmware/include/config.h` — calibrate against
  your actual resistor divider and battery chemistry. If the reported
  voltage is consistently off even after checking those (common cause: a
  100k/100k divider — ESP32's ADC systematically under-reads through source
  impedances much above ~10kΩ, since its sample-and-hold capacitor can't
  fully charge within the sampling window), recalibrate
  `BATTERY_DIVIDER_RATIO` directly: compare a multimeter reading of the
  actual battery voltage against what the device reports, then set it to
  `current_ratio * (actual_mV / reported_mV)`. Re-verify at more than one
  charge level if you can, in case the error isn't perfectly linear. For a
  proper hardware fix instead of (or in addition to) recalibrating in
  software, lower the divider resistors (e.g. 10k/10k) or add a
  ~100nF-1µF capacitor from the ADC pin to GND.
- GPS power control pin (`GPS_POWER_PIN`, `GPS_POWER_ON_LEVEL`,
  `GPS_POWER_STABILIZE_MS` in `config.h`; driven from
  `GpsReader::powerOn/powerOff` in `firmware/src/gps_reader.cpp`) - this
  assumes GPS_POWER_PIN drives Q1's gate (AO3401 P-MOSFET switching main
  VCC - see Wiring notes above) and VBAT (pin 6) is backed by the
  breakout's backup battery independently of that switched VCC rail. If
  the module doesn't come back up reliably after a wake (garbled or
  missing NMEA sentences right after power-on), try raising
  `GPS_POWER_STABILIZE_MS` - a full VCC power-on likely needs more
  settling time than the module's own ON/OFF-pin standby would have. If
  fixes after a wake are consistently as slow as a cold start (tens of
  seconds) rather than the <=1s hot-start the datasheet promises, check
  the breakout's backup battery - low or disconnected VBAT means backup
  RAM/RTC state isn't actually surviving the power cut (the case on this
  build - see TODO). If the module
  draws current or stays inactive regardless of the pin level, confirm
  Q1's Source/Drain/Gate match the wiring above (easy to get
  Source/Drain backwards on a small SOT-23 package), that `GPS_POWER_ON_LEVEL`
  is `LOW` (a P-FET's gate needs to go low to turn on - see config.h),
  and that the driving GPIO is configured `OUTPUT` before first use.
- ADXL345 activity threshold (`ADXL_ACT_THRESHOLD_MG`, default 375 mg) —
  raise it if the device wakes/stays-awake from vibration you want ignored
  (e.g. mounted on a vibrating surface), lower it if gentle movement isn't
  triggering wake reliably.
- Deep-sleep GPIO wakeup on ESP32-C6 only works on its 8 LP_IO pins
  (GPIO0-7). If you ever move `ADXL_INT_PIN` to a different pin, keep it
  within that range or motion-wake will fail at the first sleep cycle with
  `gpio X is an invalid deep sleep wakeup IO`.
- `CRITICAL_BATTERY_SLEEP_INTERVAL_SEC` (default 6h) — how often the device
  re-checks battery state once already critical at boot, before it skips
  back to sleep without bringing up WiFi/GPS. Shorter means it notices a
  newly-connected charger sooner; longer conserves more of what's left of
  a critically low cell between checks.
- `WIFI_ACTIVE_WINDOW_MS` (default 5 min) — how long WiFi/the web server
  stay on following a motion wake before being forced back off. Raise it
  if 5 minutes isn't reliably enough to connect and sync/flash OTA in your
  environment; lower it to spend less time (and battery) with the radio on
  per movement event. Doesn't apply while charging (see Battery above).
- `WIFI_RECHECK_INTERVAL_MS` (default 5 min) — during a continuous-motion
  stretch that never goes back to sleep, how often the device restarts to
  get another `WIFI_ACTIVE_WINDOW_MS` window. The restart continues the
  running track (same GPX file, inactivity clock kept - this state lives in
  `RTC_NOINIT_ATTR` memory, since the C6 bootloader reinitialises
  `RTC_DATA_ATTR` on every non-deep-sleep reset). Raise it for fewer
  restarts on long drives at the cost of fewer chances to catch a known
  network; lower it for the reverse. Doesn't apply while charging, since
  WiFi already stays on continuously there.
- `GPS_MOVEMENT_MAX_HDOP` (default 3.0) / `GPS_MOVEMENT_MIN_SATS` (default
  5) — the reception-quality bar a fix must clear before it's trusted for
  logging at all (see Logging above). Loosen (raise HDOP / lower sat
  count) if a marginal-but-genuine fix is being rejected too often in your
  environment; tighten for the reverse if indoor/multipath jitter is still
  getting logged.
- `GPS_MOVEMENT_MIN_SPEED_MPS` (default 0.5 m/s) — on top of the above,
  the minimum speed a fix must show - both position-derived (distance since
  the last logged point / time since it) and the receiver's Doppler speed -
  before it counts as real movement for the sleep decision (see Sleep
  above) - HDOP/sat count alone don't reliably catch indoor multipath on
  this receiver. Raise it
  if the device is still staying awake indoors (i.e. jitter there is fast
  enough to clear the current value); lower it if it's falling asleep
  during genuinely slow movement (e.g. walking speed on foot).
- `WIFI_SLOT_TRY_MS` (default 20 s) / `WIFI_SLOT_FAST_FAIL_MS` (default
  6 s) — how long one connection attempt to a saved network gets before
  the device moves on to the other network; the shorter value applies once
  the driver has already reported the network missing or the password
  wrong. Raise `WIFI_SLOT_TRY_MS` if your router is slow to associate.
- `WIFI_SCAN_BACKOFF_MS` (default 30 s) / `WIFI_NONE_IN_RANGE_CLOSE_MS`
  (default 45 s) — once every saved network has been reported "not found",
  the device waits this long between scan rounds instead of scanning every
  6 s, and on battery closes the WiFi window after 45 s of that rather than
  keeping the radio on for the full 5 minutes (a drive with no known WiFi
  around drained ~25 %/h before this).
- `WIFI_STUCK_RESTART_MS` (default 3 min) / `WIFI_PORTAL_STATIONARY_S`
  (default 10 min) — while charging, how long without any known network
  before the one-time recovery steps kick in (setup portal if also still
  for `WIFI_PORTAL_STATIONARY_S`, then one restart).
  `WIFI_UNCONFIGURED_PORTAL_RETRY_MS` (10 min) is how often the portal is
  re-offered on the charger when no network is saved at all.
- `CHARGE_DEBOUNCE_MS` (default 5 s) — how long a charger state change
  must hold before the plug-in restart / unplug grace window react.
- `SYNC_FS_MIN_FREE_BYTES` (default 64 KB) — margin kept free after a
  track flush; below it, the last-resort deletion of uploaded
  tracks kicks in (if enabled in Settings → Storage).
- `GPS_STILL_MAX_SPEED_MPS` (default 2 m/s) / `ADXL_ASSUME_STILL_MS`
  (default 5 s) — the stillness filter: points aren't logged while the
  ADXL says still and the GPS speed is below this; after a boot, no ADXL
  activity for `ADXL_ASSUME_STILL_MS` counts as still.
- `GPS_STOP_MAX_SPEED_MPS` (0.5 m/s) / `GPS_STOP_FIXES` (3) /
  `GPS_GO_MIN_SPEED_MPS` (1 m/s) / `GPS_GO_FIXES` (2) /
  `TRACKPOINT_STOPPED_MIN_DISTANCE_M` (50 m) / `GPS_STOPPED_FAR_FIXES` (3)
  — the stop/go filter: when the receiver's speed says "stopped", the
  minimum distance between logged points rises from 5 m to this, and the
  far position must persist for that many fixes.
- `WIFI_RECHECK_STILL_MS` (45 s) — how long the receiver must report
  stopped (or no trustworthy fix) before a WiFi window restart is allowed;
  `WIFI_MOVED_MIN_SPEED_MPS` (5 m/s) / `WIFI_STOP_WINDOW_MIN_GAP_S` (3 min)
  — after a drive at this speed the first stop opens a window at once,
  such windows at least this far apart;
  `CPU_MHZ_AWAKE` (80) — CPU clock, always;
  `WIFI_RECHECK_IDLE_INTERVAL_MS` (30 min) — recheck interval after a
  window with nothing to upload and no new points; `WIFI_IDLE_CLOSE_MS`
  (90 s) — on battery, a connected window with nothing to upload closes
  after this long without a web request (not the arrival window);
  `WIFI_ARRIVAL_WINDOW_MS` (10 min) — cap for the window opened on arrival
  after a drive, which otherwise stays up until the device sleeps.
- `SAFETY_WAKE_INTERVAL_SEC` (2 h) — timer wake with no motion, for the
  battery check (and the only wake if the accelerometer failed).
- `GPS_NOFIX_SUSPEND_AFTER_MS` (10 min) / `GPS_NOFIX_OFF_MS` (5 min) /
  `GPS_NOFIX_RETRY_ON_MS` (2 min) — the no-fix GPS duty cycle on battery.
- `GPS_DUTY_PERIOD_MS` (0 = off; 5 s when enabled) /
  `GPS_DUTY_SETTLE_FIXES` (2) / `GPS_DUTY_MIN_OFF_MS` (1.5 s) /
  `GPS_DUTY_STOPPED_OFF_MS` (30 s) — the GPS duty cycle while tracking on
  battery; disabled, since this board's GPS cold-starts after a power cut.
- `LIGHT_SLEEP_ENABLED` (0) / `LIGHT_SLEEP_SLICE_MS` (1 s) — CPU light
  sleep while the GPS module is off; disabled (ADC readings after wake).
- `CHARGER_RESTART_STABLE_MS` (30 s) / `CHARGER_RESTART_MIN_INTERVAL_S`
  (10 min) — the restart that brings WiFi up when a charger appears
  mid-cycle needs the charger present this long, and happens at most this
  often (a flickering car Qi pad or a bad reading caused a restart loop).
- `GPS_TRUSTED_SPEED_MPS` (8 km/h) / `GPS_STATIC_MIN_SATS` (6 used) /
  `GPS_STATIC_MAX_HDOP` (1.2) — the indoor-fix gate: slower fixes are only
  used with this open-sky quality.
- `MIN_SESSION_POINTS` (5) — trips with fewer points are deleted as noise.
- `STATIONARY_TIMEOUT_MIN` (7) — minutes without accelerometer activity
  (or GPS-confirmed movement) before the trip ends and the device sleeps.
- `WAKE_CONFIRM_ACTIVITY_COUNT` (3) / `UNCONFIRMED_WAKE_TIMEOUT_S` (120) /
  `WAKE_CONFIRM_MOVING_MS` (60 s) — a motion wake counts as a bump until
  this many activity interrupts, a GPS-confirmed movement, or this long in
  the ADXL's "moving" state; until then the device sleeps again this many
  seconds after the ADXL reports still.
- `ADXL_MOVING_HOLD_MAX_MS` (20 min) — while the ADXL reports continuous
  movement (no 3-s quiet, so few new activity interrupts - cycling,
  walking) the stationary timeout doesn't run, for at most this long after
  its last activity interrupt. Before, a ride could be put to sleep while
  the GPS was still cold-starting.
- `WIFI_STOPPED_OUTDOOR_MS` (2 min) / `WIFI_NOFIX_FIRST_RECHECK_MS`
  (10 min) — standing still outdoors with a good fix counts as parked
  only after this long; the regular window after a motion wake waits for
  a trustworthy fix or this long.
- `WIFI_LOST_FIX_PARKED_MS` (5 min) — no trustworthy fix counts as
  parked (indoors) only with a quiet accelerometer, and after a fix lost
  at driving speed only after this long.
- `SYNC_MAX_UPLOAD_FAILURES` (10) / `SYNC_GIVEUP_RETRY_MS` (10 min) —
  consecutive probe/upload failures before sync pauses, and for how long
  after upload failures; `SYNC_NO_BACKEND_RETRY_MS` (2 min) is the pause
  when no backend answered the probe at all, `SYNC_PROBE_CONNECT_MS` (5 s)
  the probe's connect timeout and `SYNC_PROBE_RETRY_MS` (15 s) the delay
  before re-probing after a failed round.
- `CRITICAL_RESUME_MARGIN_PERCENT` (default 3) — after a critical-battery
  sleep, how far above `CRITICAL_BATTERY_PERCENT` the battery must read
  before normal operation resumes.
- WiFi connect retries/timeout during setup (`wm.setConnectRetries`/
  `setConnectTimeout` in `ConfigManager::runCaptivePortal`) — bump these if
  your router is slow to associate; see the troubleshooting note in Part 4
  above for the more common causes (5GHz-only/band-steering, WPA3/PMF).
- Buzzer timing (`LOW_BATTERY_BEEP_COUNT`, `LOW_BATTERY_BEEP_ON_MS/OFF_MS`,
  `LOW_BATTERY_BEEP_INTERVAL_MS`; for the storage warning `STORAGE_BEEP_*`
  incl. the daytime window `STORAGE_BEEP_DAY_START_HOUR`/`_END_HOUR`, and
  the thresholds `STORAGE_WARN_BYTES`/`STORAGE_CRITICAL_BYTES`) and drive
  polarity (`Buzzer::setBuzzer` assumes active-high) — adjust for your
  buzzer/driver hardware. To hear the storage-warning pattern without
  filling the flash, build once with the test flag (it forces the alert on:
  3 long beeps right after boot, then every 30 min), then flash a normal
  build again:
  ```bash
  PLATFORMIO_BUILD_FLAGS="-D STORAGE_BEEP_TEST=1" pio run -t upload
  ```
- `partitions.csv` assumes 4MB flash; adjust offsets/sizes for other flash sizes.
- `platformio.ini` pins the `pioarduino` platform fork (needed for ESP32-C6 +
  Arduino core 3.x support) - if you later bump the release URL, make sure
  you don't mix it with `platform = espressif32` in the same project/cache,
  since the two use incompatible build scripts (see the build step above).
- The REST API currently uses plain HTTP for simplicity on a home LAN; put
  the backend behind your router's firewall (no port-forward to the
  internet) or add TLS + auth if you need remote (off-LAN) access.

## TODO — pending hardware changes

Two small soldering jobs, both identified by measurement on the current
build. None needs a firmware change - the code already handles both
configurations; these make existing features reliable/tunable.

- [ ] **GPS backup power: wire the module's VBAT (pin 6) to the XIAO's
  3V3 pin** (instead of, or in parallel via a diode with, the breakout's
  own backup battery, which doesn't hold the receiver's state on this
  build). The XIAO's 3V3 stays powered in deep sleep, so the GPS keeps
  its clock and satellite data through every VCC cut: a fix in about a
  second after short gaps and ~30 s after a night, instead of the 3-5.5
  minutes now - the start of every trip is currently missing from the
  track. The backup domain draws only a few to tens of µA (check the
  ATGM336H datasheet) - added to the deep-sleep current. Verify with the
  debug log: `[GPS] First fix N s after power-on` after a short stop
  should drop to a few seconds. That also makes the implemented GPS duty
  cycle (`GPS_DUTY_PERIOD_MS`) worth re-enabling.

- [ ] **ADXL345 supply decoupling: 100nF + 10uF directly at the module's
  VCC/GND pins** (as close as possible; short/twisted supply+ground leads
  help too). Measured noise floor on a completely still board is 60-110mg
  sample-to-sample (~20x the datasheet), spiking >200mg during WiFi TX
  bursts - which is why `ADXL_ACT_THRESHOLD_MG` currently sits at 375 and
  gentle handling doesn't register. After adding the caps, rebuild once
  with `-D MOTION_DEBUG_RAW=1` and watch the `[Motion] peak sample delta`
  lines with the board untouched: if the floor drops to a few tens of mg,
  lower `ADXL_ACT_THRESHOLD_MG` from 375 toward 125 for pick-it-up
  sensitivity (and `ADXL_INACT_THRESHOLD_MG` with it - keep it one 62.5 mg
  step below, see its comment in config.h). If the floor does NOT drop, suspect the breakout itself -
  this noise signature is also typical for counterfeit ADXL345s; try a
  replacement module.
