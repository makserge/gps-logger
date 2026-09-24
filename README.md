# Offline GPS Logger

Battery-powered GPS tracker built on ESP32-C6 + UART GPS module, with local
RAM buffering, periodic flash persistence as GPX, WiFi auto-sync to a
Raspberry Pi / Home Assistant backend over REST (tracks + live status), a
captive-portal WiFi/config manager, OTA firmware updates, and a simplified
on-device web UI plus a fuller backend web UI for browsing tracks by
calendar date.

## Step-by-step setup

Follow this in order for a first-time build. Reference sections with full
detail (pin table, API, tuning notes) are further down.

### Part 1 — Hardware wiring

1. Connect the GPS module (ATGM336H) to the ESP32-C6:
   - GPS TX → ESP32-C6 D10 (GPIO18, RX)
   - GPS RX → ESP32-C6 D9 (GPIO20, TX)
   - **GPS VCC → Drain of Q1 (AO3401 P-MOSFET)**, not straight to 3.3V -
     main VCC is switched, not left always-on. Q1's Source → 3.3V, Gate →
     ESP32-C6 D5 (GPIO23) through a 150Ω series resistor (R1), with a 10KΩ
     pull-up (R2) from the gate node to 3.3V so the switch defaults OFF if
     the GPIO is left floating (e.g. during boot before pinMode() runs).
     Pin 5 (ON/OFF) is not used in this build. Backup power for the
     RTC/SRAM domain comes from a separate pin (VBAT, pin 6) which this
     breakout backs with its own small battery, independently of the
     switched main VCC rail - so cutting VCC should still give a fast
     hot-start wake (<=1s TTFF per datasheet) rather than a cold reacquire
     (<=35s), same as shutting the module off via pin 5 would. See Wiring
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
   from the pad the same way it would from a cable. Optionally add a
   resistor divider from the 5V pin to D0 (GPIO0) to detect charging status
   in the web UI (see Wiring notes below - the XIAO has no dedicated CHRG pin).
4. **Battery voltage sense:** wire a voltage divider from battery+ to D1
   (GPIO1/A1, ADC). Use two similar-value resistors (e.g. 100k/100k) so the
   divided voltage stays under 3.3V at full charge — note the exact ratio,
   it's needed in step 5.

### Part 2 — Firmware setup

5. Install PlatformIO (VS Code extension, or CLI: `pip install platformio`).
6. Calibrate battery constants in `firmware/include/config.h`:
   - Measure your actual divider ratio and adjust `BATTERY_DIVIDER_RATIO`.
   - Adjust `BATTERY_ADC_MAX_MV` / `BATTERY_ADC_MIN_MV` to your battery's
     real full/empty voltage as measured at the ADC pin.
   - If your ADXL345 breakout ties SDO/ALT-ADDR high, change
     `ADXL_I2C_ADDR` to `0x1D`.
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
   repeatedly** while you're actively developing over USB, that's most
   likely deep sleep, not a crash - the XIAO's native USB CDC fully resets
   on every deep-sleep wake cycle (inherent to the chip), and the board
   sleeps automatically after 15 minutes with no motion. If you also see
   `I2C transaction unexpected nack detected` in the log, that's just the
   ADXL345 not responding (harmless if it isn't wired up yet - the driver's
   own error logging is silenced by default now, so if you still see it,
   you're on an older build). For a stable connection while bench-testing,
   build with the `-dev` environment instead, which disables deep sleep
   entirely:
   ```bash
   pio run -e esp32-c6-devkitm-1-dev -t upload
   ```
   Switch back to the normal environment (`pio run -t upload`, or explicitly
   `-e esp32-c6-devkitm-1`) before deploying it battery-powered in the field.
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
13. Choose your home WiFi network and enter its password.
14. Fill in the extra fields: backend host/IP and REST port (8080).
15. Save — the device reboots and joins your WiFi.

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

    **To reconfigure WiFi later** (new router, moving the device to a new
    network, etc.): open `http://<device-ip>/` and click **Reset WiFi** -
    this erases only the saved WiFi credentials (backend settings are
    kept) and reboots straight into the setup portal. This only works while
    the device is currently reachable on *some* WiFi network, though - if
    it's already offline and unreachable (e.g. the old network's password
    changed while the device was out of range), fall back to a full erase
    and reflash instead:
    ```bash
    pio run -t erase   # wipes flash, including the saved WiFi credentials
    pio run -t upload
    ```
    (This fallback also wipes any logged-but-unsynced GPX sessions on the
    device, so sync first if that matters and it's reachable.)

### Part 5 — Verify it's working

16. Find the device's IP (check your router's client list, or watch serial
    monitor output).
17. Open `http://<device-ip>/` in a browser — you should see the simplified
    on-device UI with battery %, charging state, RAM usage, WiFi/sync status,
    and (once you've moved around) a calendar with GPX sessions to download.
18. Open the backend UI at `http://<backend-ip>:8080/` — pick your device
    from the dropdown, browse dates, view tracks on the map, download GPX,
    and check the live device-status line (reported by the device over HTTP
    whenever it is awake with WiFi).
19. Take the device for a walk/drive — confirm points log (every ≥3s, ≥5m
    movement), and that a flush hits flash after 15 min or on a forced sync.
20. Leave it stationary for 15+ minutes with no vibration and confirm (via
    serial monitor) that it deep-sleeps once the ADXL345 stops reporting
    activity; tap/move the device and confirm it wakes immediately via the
    motion interrupt rather than waiting on a timer.
21. Drain (or simulate, by editing `LOW_BATTERY_PERCENT` temporarily) a low
    battery condition and confirm the buzzer beeps 5 times, repeating once a
    minute until charged or critical.

### Part 6 — OTA updates going forward

22. Build a new firmware binary:
    ```bash
    pio run
    ```
    The output lands at `.pio/build/esp32-c6-devkitm-1/firmware.bin`.
23. Push it over WiFi instead of USB - either:
    - Open `http://<device-ip>/`, use the "Firmware update" panel, pick the
      `.bin` file, and hit Flash (shows upload/flash progress, then a
      verification check and reboot countdown), or
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
  Dockerfile              offline install from wheels/ (works for both paths)
  docker-compose.yml      standalone `docker compose up -d --build` path
  config.yaml             HA local app manifest - copy this whole folder into
                           HA's /addons/ directory to run it as a custom App
  wheels/                 vendored deps + update-wheels.sh to regenerate them
```

## Hardware

- **Seeed Studio XIAO ESP32-C6** (PlatformIO env is named `esp32-c6-devkitm-1`
  for historical reasons, but `board =` inside it is the proper
  `seeed_xiao_esp32c6` definition, not the generic Espressif devkit one -
  the XIAO has its own onboard battery power management, so no separate
  charge IC is needed - see below)
- UART GPS module: **ATGM336H** at 9600 baud (sleep/wake handled entirely
  in software over UART - see below - no power-gate hardware needed)
- ADXL345 accelerometer breakout (I2C) - used for motion-interrupt wake
- Active 3.3V buzzer for low-battery audible alerts
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
| GPS RX (ESP32 RX)         | D10      | 18   |
| GPS TX (ESP32 TX)         | D9       | 20   |
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
  that domain should stay alive even with VCC fully cut. Per the
  datasheet's own TTFF spec, that means wake
  should land in the hot-start bucket (<=1s) rather than cold-start
  (<=35s) - worth confirming in practice by comparing time-to-fix after a
  wake against a genuine first-ever cold boot.
- **ADXL345**: SDA→D7/GPIO17, SCL→D8/GPIO19, INT1→D2/GPIO2, VCC→3.3V,
  GND→GND. Leave INT2 unconnected. SDA/SCL sit next to the GPS wiring
  (D9/D10) for short runs to both modules; INT1 has to stay on D2 instead
  of joining them, though - ESP32-C6 only supports deep-sleep GPIO wakeup
  on its LP_IO pins (GPIO0-7), and D7-D10 (GPIO17/19/20/18) aren't in that
  range, so putting INT1 there would silently break motion-wake at the
  first sleep cycle. If your breakout ties the address-select pin
  (SDO/ALT ADDRESS) high, change `ADXL_I2C_ADDR` to `0x1D` in `config.h`
  (default assumes it's tied low → `0x53`).
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
  alone. Add a small resistor divider (e.g. two ~100k resistors) from the
  5V pin down to D0/GPIO0 (~2.5V when powered, safely under the 3.3V
  logic-input max) and firmware reads that as a digital HIGH = charging.
  This is surfaced in the web UI and the device-status payload the device
  POSTs to the backend. (Firmware actually samples this pin via the ADC
  rather than digitalRead - 2.5V sits exactly on the chip's digital
  threshold - and for a reliable plug-in wake from deep sleep, use ~100k
  top / ~180k bottom instead, giving ~3.2V - see config.h.)
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
- **Flash persistence**: buffer is flushed to a per-session GPX file on
  LittleFS every 15 minutes, on low battery (≤15%), before sleep, and before
  ending a session. The GPX file is kept valid after every flush (footer is
  rewritten each time), so a sudden power loss never corrupts it.
- **Sessions**: one GPX file per "trip" (from boot until the device decides
  to sleep due to inactivity), stored under `/tracks/YYYY-MM-DD/<epoch>.gpx`
  — this is what shows up in both the on-device and backend calendar views.
  The session's date/filename is assigned lazily, on the first real GPS
  point, rather than eagerly at boot - the system clock isn't set until
  GPS delivers its first fix, so starting a session any earlier would
  create a spurious `1970-01-01` folder. A trip that spans a deep-sleep
  wake cycle (motion pauses, then resumes) continues appending to the
  same GPX file rather than fragmenting into multiple files - the
  session's start time is persisted across sleep and used to
  deterministically reconstruct its file path on each wake.
- **Sleep (motion-interrupt driven)**: the ADXL345 is configured for
  hardware activity detection (~0.3g threshold) on its INT1 pin, which is
  wired to a GPIO. While awake, every activity pulse resets an inactivity
  timer; if no motion has been detected for 15 minutes, the device flushes,
  cuts power to the GPS module via its hardware switch (D5/GPIO23), also
  releases the GPS UART and I2C (accelerometer) peripherals and tri-states
  their pins (`GpsReader::end()`/`MotionSensor::end()` - INPUT, no pull;
  the ADXL's own interrupt pin is left alone since it's the wake source
  itself) so nothing keeps driving current into an now-unpowered peripheral
  through its input protection diodes, and deep-sleeps. Both peripherals
  come back up automatically on the next wake via their normal begin()
  calls in setup(), since deep sleep re-runs the whole boot from scratch.
  Unlike a periodic GPS-coordinate check, the device does
  **not** wake on a fixed schedule to take a GPS fix just to see if it
  moved - it stays asleep until the ADXL345 itself interrupts the ESP32 on
  real motion (µA-level sensor, vs. powering up GPS every minute), which is
  a substantial power saving.
- **GPS is gated by motion**: it's only powered on and tracked on a fresh
  boot or a wake actually caused by the ADXL's motion interrupt - a
  safety-net timer wake (default hourly, `SAFETY_WAKE_INTERVAL_SEC`) with
  no motion detected skips GPS entirely and is purely a battery-level check
  before going straight back to sleep.
- **WiFi and the web server are also gated by motion**, on the same
  fresh-boot-or-motion-wake trigger as GPS, but on a bounded timer rather
  than for the whole active period: once brought up, WiFi is forced back
  off again after `WIFI_ACTIVE_WINDOW_MS` (5 minutes by default) regardless
  of whether motion continues, since it's meant as a periodic sync/OTA
  opportunity rather than something that needs to stay on continuously - a
  safety-timer-only wake never brings WiFi up at all. This means **the
  device is normally only reachable over WiFi/OTA for a few minutes
  following actual movement** (or via the captive portal when WiFi isn't
  configured yet) - trading away always-on remote reachability for
  substantially better battery life, since WiFi is one of the biggest power
  draws on this chip. The exception is charging: WiFi also activates
  whenever the charger is present, independent of motion, and the 5-minute
  cutoff doesn't apply while charging either - deep sleep is already
  skipped entirely then (see Battery below), so there's no power-saving
  reason to force WiFi off, and it's what lets NTP sync (see below) work at
  all while sitting stationary on a charging pad. There's no physical wake
  button in this design - the WiFi/backend setup portal opens automatically
  only when no WiFi is configured at all, or on demand via the "Reset
  WiFi" link in the on-device web UI while it's reachable (see "To
  reconfigure WiFi later" in Part 4 above).
- **Battery**: at ≤15% the buffer is force-flushed on every low reading, and
  a buzzer sounds 5 short beeps, repeating once a minute for as long as the
  battery stays low (it stops immediately if you plug in/place the device on
  the Qi pad and charging is detected, or once the battery is critical - see
  below). At ≤5% the session is closed and the device sleeps (unless
  charging - see below). If the battery is *already* critical at boot
  (e.g. an hourly safety wake finds it still critical) and not charging,
  the device skips WiFi, GPS, and every other peripheral entirely and goes
  straight back to sleep for a much longer interval
  (`CRITICAL_BATTERY_SLEEP_INTERVAL_SEC`, 6h by default) instead of the
  normal hourly cadence - protecting the cell from further drain takes
  priority over remote reachability in this state, so the device will
  *not* be reachable over WiFi/OTA while critical and unplugged. It
  re-checks every 6h and resumes normal behavior automatically once
  charged back above the threshold.
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
- **WiFi**: on boot, the device tries to join the previously configured
  network in station mode. If none is configured (or the button is held 3 s
  at boot), it starts a `WiFiManager` captive portal AP
  (`GPSLogger-Setup-XXXX`) where you set WiFi credentials **and** the
  backend host/port in one flow (both changeable later from the on-device
  web UI via the "Backend..." link, without re-running the portal).
- **Sync**: whenever WiFi is connected, the device periodically (every 30 s)
  checks for unsynced sessions, PUTs them to the backend's REST API, and
  POSTs its live status. Sync is fully automatic (immediately on wake, every
  30 s while connected, and before sleep) - there is deliberately no remote
  "sync now" channel, since a sleeping device is unreachable anyway and an
  awake one is already syncing.
- **OTA**: the on-device web UI has a "Firmware update" panel - pick a
  `firmware.bin` and hit Flash; a progress bar tracks the upload (which
  writes to flash synchronously as it streams in), followed by a
  verification check against the device and a countdown while it reboots.
  Under the hood it's `POST /update` with a multipart firmware binary
  (standard ESP32 `Update` library flow), so it can still be scripted
  directly if preferred:
  `curl -F "firmware=@.pio/build/esp32-c6-devkitm-1/firmware.bin" http://<device-ip>/update`.
  Remember the device is only reachable during its bounded WiFi window
  (movement, or charging - see "Sleep" above), so have it moving or on the
  charger when you want to push an update.
- **On-device web UI**: `http://<device-ip>/` shows battery/WiFi/sync
  status, GPS fix/satellite count, board time (`Date: dd/mm/yyyy | Time:
  hh:mm:ss`, in local time with a DST indicator when active), a timezone
  selector, a "Reset WiFi" link to reconfigure the network (see Part 4), an
  OTA firmware flash panel with progress, and a calendar list of sessions
  with download and delete links (per-session, or "delete date" for a
  whole day at once - useful for clearing out unwanted data, e.g. a stray
  pre-GPS-fix date folder, without a full flash erase).
- **Timezone**: set from a dropdown of common zones (or a custom POSIX TZ
  string, e.g. `CET-1CEST,M3.5.0,M10.5.0/3`) in the web UI. This uses the C
  library's own DST engine - the string encodes both the base UTC offset
  and the DST transition rule, so summer/winter time switches automatically
  on the right dates with no manual twice-a-year toggling. It affects the
  web UI's displayed board time, the device-status payload, and session
  date-folders/filenames (so trips are grouped and named by local calendar
  day, not UTC day - a trip starting at 11pm local wouldn't otherwise get
  filed under "tomorrow"). It never touches the actual `<trkpt><time>`
  values inside GPX files, though - those stay UTC always, per the GPX
  spec, for correct interchange with other tools.

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
4. Supervisor finds `config.yaml` and places "GPS Logger Backend" at the very top under **Local Apps**.
5. Select it, click **Install**, and wait for the hardware compilation step to finish.

It stores tracks under HA's `/share/gpslogger/tracks` and exposes its web UI via HA's Ingress (shows up in the sidebar) as well as directly on port 8080 for the ESP32 devices to reach. There are no options to configure.

**Note:** the vendored `backend/wheels/` currently only cover aarch64
(matching a Raspberry Pi / HA-on-aarch64 host). `config.yaml` lists `amd64`
as a supported arch too, but the build will actually fail there until you
regenerate the wheels for amd64 with `backend/wheels/update-wheels.sh`
(needs internet - the Docker/Supervisor build itself does not have it).

## Compatibility with existing tracking tools

::content::Sessions are stored as standard **GPX 1.1** files with `<trkpt>` elements
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
| GET | `/tiles/{z}/{x}/{y}.png` | Cached map-tile proxy for the web UI |

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
  RAM/RTC state isn't actually surviving the power cut. If the module
  draws current or stays inactive regardless of the pin level, confirm
  Q1's Source/Drain/Gate match the wiring above (easy to get
  Source/Drain backwards on a small SOT-23 package), that `GPS_POWER_ON_LEVEL`
  is `LOW` (a P-FET's gate needs to go low to turn on - see config.h),
  and that the driving GPIO is configured `OUTPUT` before first use.
- ADXL345 activity threshold (`ADXL_ACT_THRESHOLD_MG`, default 300 mg) —
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
- WiFi connect retries/timeout (`wm.setConnectRetries`/`setConnectTimeout` in
  `ConfigManager::runCaptivePortal`) — bump these if your router is slow to
  associate; see the troubleshooting note in Part 4 above for the more
  common causes (5GHz-only/band-steering, WPA3/PMF).
- Buzzer timing (`LOW_BATTERY_BEEP_COUNT`, `LOW_BATTERY_BEEP_ON_MS/OFF_MS`,
  `LOW_BATTERY_BEEP_INTERVAL_MS`) and drive polarity (`Buzzer::setBuzzer`
  assumes active-high) — adjust for your buzzer/driver hardware.
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
build. Neither needs a firmware change - the code already handles both
configurations; these make existing features reliable/tunable.

- [ ] **Charge-detect divider: swap the bottom resistor 100k → 180k.**
  The divider from the 5V pin to D0/GPIO0 currently uses 100k/100k, putting
  ~2.5V on the pin - almost exactly the C6's digital V_IH (0.75 × 3.3V =
  2.475V), i.e. zero noise margin. Awake-side detection works anyway
  (firmware samples the pin via ADC), but the deep-sleep plug-in wake
  (EXT1) compares against the digital threshold and is only marginally
  reliable. With 100k top / 180k bottom the pin sits at ~3.2V - solid
  margin, still within pad limits at max USB voltage. After the swap,
  verify: unplug → let it sleep (15+ min still) → plug in gently → device
  must wake within seconds, twice in a row, with the boot log saying
  `Woke via USB/charger power appearing` (not the motion wake).

- [ ] **ADXL345 supply decoupling: 100nF + 10uF directly at the module's
  VCC/GND pins** (as close as possible; short/twisted supply+ground leads
  help too). Measured noise floor on a completely still board is 60-110mg
  sample-to-sample (~20x the datasheet), spiking >200mg during WiFi TX
  bursts - which is why `ADXL_ACT_THRESHOLD_MG` currently sits at 375 and
  gentle handling doesn't register. After adding the caps, rebuild once
  with `-D MOTION_DEBUG_RAW=1` and watch the `[Motion] peak sample delta`
  lines with the board untouched: if the floor drops to a few tens of mg,
  lower `ADXL_ACT_THRESHOLD_MG` from 375 toward 125 for pick-it-up
  sensitivity. If the floor does NOT drop, suspect the breakout itself -
  this noise signature is also typical for counterfeit ADXL345s; try a
  replacement module.
