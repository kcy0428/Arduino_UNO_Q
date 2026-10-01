# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Arduino App for the **UNO Q** (`arduino,imola`) that reads three VL53L8CX 8×8 ToF sensors and shows them as heatmaps in a web UI. Sensors 1-2 share one I²C bus (`Wire`, D20/D21); sensor 3 is on its own bus (`Wire2`, A4/A5) — see `tof3-i2c-notes.md` §2.1 for why. User-facing docs (README) are in Korean; keep UI strings and README in Korean.

## Commands

There is no build system, linter, or test suite — the app is built and run by `arduino-app-cli`. `app start` stops whatever app is currently running, so confirm with the user first.

```sh
arduino-app-cli app start   /home/arduino/ArduinoApps/tof3-i2c
arduino-app-cli app logs    /home/arduino/ArduinoApps/tof3-i2c --follow   # Python print() output
arduino-app-cli monitor                                                   # sketch Serial.println output
arduino-app-cli app clean-cache user:tof3-i2c --force                     # if sketch build seems stale
curl -s http://localhost:7000/api/status                                  # JSON snapshot of all 3 sensors
```

Healthy state: each sensor reports `measuring` and its frame `seq` increases (Python logs a summary line per sensor every 10 s).

## Architecture

Three layers, joined by string names that must match exactly:

1. **Sketch (`sketch/sketch.ino`, MCU)** — owns the I²C bus(es) and the sensors.
   - Bus assignment: `SENSOR_BUS[] = {&Wire, &Wire, &Wire2}` — sensors 1-2 share `Wire`, sensor 3 gets `Wire2` alone. `writeSensor`/`readSensor` take a `SensorState*` handle and read `sensor->bus`, so every I²C call (both the sketch's direct calls and the ST driver's internal calls via `platform.handle`) goes out the right bus. Adding a 4th sensor or reassigning buses means editing this array and nothing else in the I²C plumbing.
   - Addressing: all LPn pins (D2/D3/D4) are driven LOW at boot, then each sensor is raised in turn and moved from the default `0x29` to `0x30/0x31/0x32` (7-bit; the ST driver stores addresses shifted left by 1, hence the `<< 1` / `>> 1` throughout). Only sensors sharing a bus can actually collide; sensor 3 keeps the same addressing dance for code uniformity even though it doesn't need to.
   - LPn only isolates I²C — it does not reset the chip, so a previously assigned address survives. `connectSensor()` therefore probes `0x29` first, then falls back to the assigned address (`alreadyAssigned`). Identity check reads bank 0 regs 0/1 expecting `0xF0/0x0C`, and must switch back to bank 2 afterward.
   - Custom `writeSensor`/`readSensor` platform callbacks chunk transfers to 128 bytes (Wire buffer limit); the VL53L8CX firmware upload depends on this.
   - Per-sensor state machine (`waiting → checking → initializing → measuring`, or `error`). Failures pull that sensor's LPn LOW and retry after `RETRY_INTERVAL_MS` (3 s) without affecting the others. A status heartbeat is re-sent every 2 s. Boot/LPn settle timing (`BOOT_SETTLE_MS`, `LPN_SETTLE_MS`, `RETRY_INTERVAL_MS`) was trimmed from more conservative defaults (2000/100/10000 ms) for faster startup and reconnects — if the shared I²C bus turns out to be signal-marginal (see below), raise these back up before suspecting the logic.
   - A zone's distance is sent as `null` unless a target is detected with status 5 or 9.
2. **Bridge** — sketch → Python `notify` only (fire-and-forget, so a name mismatch fails silently):
   - `tof3_status(index, phase, detail, code)`
   - `tof3_frame(index, payload)` where `payload` is a JSON string `{"seq", "distance_mm"[64], "target_status"[64]}`.
3. **Python (`python/main.py`)** — validates frames, keeps per-sensor state under a lock, and exposes `GET /api/status` via the `arduino:web_ui` brick (port 7000). A sensor is `live` only if phase is `measuring` and the last frame is < 3 s old.
4. **Web UI (`assets/`)** — plain JS polling `/api/status`; renders heatmaps, optional horizontal flip, and per-sensor CSV export (zones in original row-major order).

## Cross-file invariants

- `SENSOR_COUNT = 3`, the address base `0x30`, and LPn pins `D2..D4` are duplicated in `sketch.ino`, `main.py` (`empty_sensor` address), and `assets/app.js` (card count, address label, `D${index + 2}`, and the `sensors.length !== 3` check). Change them together, along with the README wiring table.
- Library versions are pinned in `sketch/sketch.yaml` (`STM32duino VL53L8CX`, `Arduino_RouterBridge` and its dependencies); there is no separate library install step.
