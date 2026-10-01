import json
import threading
import time

from arduino.app_utils import App, Bridge
from arduino.app_bricks.web_ui import WebUI

SENSOR_COUNT = 3
web = WebUI()
lock = threading.Lock()


def empty_sensor(index):
    return {"id": index, "address": f"0x{0x30 + index:02X}", "phase": "waiting",
            "detail": "MCU 연결 대기 중", "error_code": 0, "seq": 0,
            "distance_mm": [None] * 64, "target_status": [0] * 64,
            "last_frame": None, "last_status": None}


sensors = [empty_sensor(i) for i in range(SENSOR_COUNT)]
last_log = [0.0] * SENSOR_COUNT


def on_status(index: int, phase: str, detail: str, code: int):
    if not isinstance(index, int) or not 0 <= index < SENSOR_COUNT:
        return
    with lock:
        sensor = sensors[index]
        changed = (phase, detail, code) != (sensor["phase"], sensor["detail"], sensor["error_code"])
        sensor.update(phase=phase, detail=detail, error_code=code,
                      last_status=time.monotonic())
    if changed:
        print(f"[ToF {index + 1}] {phase}: {detail} (code={code})", flush=True)


def on_frame(index: int, payload: str):
    if not isinstance(index, int) or not 0 <= index < SENSOR_COUNT:
        return
    try:
        frame = json.loads(payload)
        distances = frame["distance_mm"]
        statuses = frame["target_status"]
        seq = frame["seq"]
        if len(distances) != 64 or len(statuses) != 64:
            raise ValueError("Expected 64 zones")
        if not isinstance(seq, int) or seq < 0:
            raise ValueError("Invalid sequence")
        if any(v is not None and (not isinstance(v, int) or v < 0) for v in distances):
            raise ValueError("Invalid distance")
        if any(not isinstance(v, int) or not 0 <= v <= 255 for v in statuses):
            raise ValueError("Invalid target status")
        now = time.monotonic()
        with lock:
            sensors[index].update(seq=seq, distance_mm=distances,
                                  target_status=statuses, last_frame=now)
        if now - last_log[index] >= 10:
            valid = [v for v in distances if v is not None]
            print(f"[ToF {index + 1}] frame={seq} valid={len(valid)}/64 "
                  f"min_mm={min(valid) if valid else None} "
                  f"max_mm={max(valid) if valid else None}", flush=True)
            last_log[index] = now
    except (ValueError, KeyError, TypeError) as exc:
        print(f"[ToF {index + 1}] Rejected malformed frame: {exc}", flush=True)


def snapshot():
    now = time.monotonic()
    with lock:
        result = []
        for sensor in sensors:
            data = {key: value for key, value in sensor.items()
                    if key not in ("last_frame", "last_status")}
            frame_time = sensor["last_frame"]
            status_time = sensor["last_status"]
            data["frame_age_s"] = None if frame_time is None else round(now - frame_time, 2)
            data["mcu_age_s"] = None if status_time is None else round(now - status_time, 2)
            data["live"] = (frame_time is not None and now - frame_time < 3
                            and sensor["phase"] == "measuring")
            result.append(data)
    return {"sensors": result}


Bridge.provide("tof3_status", on_status)
Bridge.provide("tof3_frame", on_frame)
web.expose_api("GET", "/api/status", snapshot)
print("ToF 3 I2C Monitor: http://<board-ip>:7000", flush=True)
App.run()
