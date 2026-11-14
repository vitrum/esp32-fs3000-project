#!/usr/bin/env python3
"""Local HTTP playback of dual-IMU CSV using the shared export estimator."""

from __future__ import annotations

import argparse
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import threading
from urllib.parse import parse_qs, urlsplit
import uuid

import matplotlib

matplotlib.use("Agg")
from matplotlib.figure import Figure
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from imu_to_bvh_hdf5 import estimate_orientations, load_csv

MAX_UPLOAD = 64 * 1024 * 1024
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/viewer.js": ("viewer.js", "text/javascript; charset=utf-8"),
    "/scene.js": ("scene.js", "text/javascript; charset=utf-8"),
    "/bike_model.js": ("bike_model.js", "text/javascript; charset=utf-8"),
    "/test_scene.js": ("test_scene.js", "text/javascript; charset=utf-8"),
    "/vendor/three.module.min.js": ("vendor/three.module.min.js", "text/javascript"),
    "/vendor/three.core.min.js": ("vendor/three.core.min.js", "text/javascript"),
    "/vendor/OrbitControls.js": ("vendor/OrbitControls.js", "text/javascript"),
}


def rotation(angles):
    """Sensor-native XYZ Euler angles, composed as Rz(yaw) Ry(pitch) Rx(roll)."""
    roll, pitch, yaw = np.radians(angles)
    cr, sr = np.cos(roll), np.sin(roll)
    cp, sp = np.cos(pitch), np.sin(pitch)
    cy, sy = np.cos(yaw), np.sin(yaw)
    return np.array([
        [cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
        [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
        [-sp, cp * sr, cp * cr],
    ])


def levelled_motion(samples):
    """Reuse shared tilt, integrating yaw about world up rather than native Z."""
    reference = next(sample for sample in samples if sample.accepted_pair)
    motion = [{} for _ in samples]
    for sensor, valid_attr, time_attr, orientation_attr in (
        ("board", "board_valid", "board_time_ms", "board_orientation"),
        ("wt", "wearable_valid", "wearable_time_ms", "wearable_orientation"),
    ):
        previous_time = None
        heading = 0.0
        reference_heading = None
        reference_tilt = rotation((*getattr(reference, orientation_attr)[:2], 0))
        estimates = []
        for sample in samples:
            if not getattr(sample, valid_attr):
                estimates.append(None)
                continue
            tilt = rotation((*getattr(sample, orientation_attr)[:2], 0))
            timestamp = getattr(sample, time_attr)
            if previous_time is not None:
                dt_ms = (timestamp - previous_time) & 0xFFFFFFFF
                if 0 < dt_ms <= 1000:
                    gyro = np.array([sample.values[f"{sensor}_g{axis}_dps"] for axis in "xyz"])
                    heading += float(tilt[2] @ gyro) * dt_ms / 1000
            if previous_time is None or dt_ms < 0x80000000:
                previous_time = timestamp
            estimates.append((tilt, heading))
            if sample is reference:
                reference_heading = heading
        for result, estimate in zip(motion, estimates):
            if estimate is not None:
                tilt, heading = estimate
                change = heading - reference_heading
                result[sensor] = (rotation((0, 0, change)) @ tilt @ reference_tilt.T, change)
    return motion


def analyze(raw: bytes):
    with tempfile.TemporaryDirectory(prefix="imu-web-") as directory:
        path = Path(directory) / "upload.csv"
        path.write_bytes(raw)
        _, samples, _ = load_csv(path)
    rejected = {"board": 0, "wt": 0}
    for sample in samples:
        for sensor, valid_attr in (("board", "board_valid"), ("wt", "wearable_valid")):
            if getattr(sample, valid_attr):
                norm = math.hypot(*(sample.values[f"{sensor}_a{axis}_g"] for axis in "xyz"))
                if norm < 0.05:
                    setattr(sample, valid_attr, False)
                    rejected[sensor] += 1
        sample.accepted_pair = sample.accepted_pair and sample.board_valid and sample.wearable_valid
    valid = [sample for sample in samples if sample.accepted_pair]
    if not valid:
        raise ValueError("No synchronized pairs with usable acceleration (>= 0.05 g)")
    estimate_orientations(samples, 0.5)
    reference = valid[0]
    motion = levelled_motion(samples)
    frames = []
    for sample, orientation in zip(samples, motion):
        matrices = [orientation[sensor][0] if sensor in orientation else None for sensor in ("board", "wt")]
        angle = None
        if sample.accepted_pair:
            relative = matrices[0].T @ matrices[1]
            angle = math.degrees(math.acos(float(np.clip((np.trace(relative) - 1) / 2, -1, 1))))
        frames.append({
            "time_ms": sample.elapsed_ms,
            "paired": sample.accepted_pair,
            "board": None if matrices[0] is None else matrices[0].tolist(),
            "wt": None if matrices[1] is None else matrices[1].tolist(),
            "board_heading_change_deg": orientation["board"][1] if "board" in orientation else None,
            "wt_heading_change_deg": orientation["wt"][1] if "wt" in orientation else None,
            "relative_angle_deg": angle,
            "skew_ms": sample.skew_ms,
            "values": {key: value if math.isfinite(value) else None for key, value in sample.values.items()},
        })
    angles = [frame["relative_angle_deg"] for frame in frames if frame["relative_angle_deg"] is not None]
    summary = {
        "rows": len(samples), "paired_rows": len(valid),
        "unpaired_rows": len(samples) - len(valid),
        "duration_s": samples[-1].elapsed_ms / 1000,
        "low_acceleration_rejected": rejected,
        "relative_angle_deg": {"min": min(angles), "max": max(angles), "mean": sum(angles) / len(angles)},
        "reference_time_ms": reference.elapsed_ms,
        "method": "Shared BVH complementary tilt, tau=0.5s; gyro projected onto levelled world up for heading; first paired pose zeroed",
        "heading_reference": "manual initial heading; magnetic north is an uncalibrated reference, not measured",
        "limitations": "No magnetometer/calibration recorded. Heading drifts; translation requires an illustrative cockpit/arm constraint model, not measured positioning. Sensor mounting axes remain uncalibrated.",
    }
    return {"summary": summary, "frames": frames}


def render_frame(data, index: int, elevation=25.0, azimuth=-60.0, wearable=(15.0, -10.0, 0.0)):
    frame = data["frames"][index]
    figure = Figure(figsize=(9, 6), dpi=95, layout="constrained")
    ax = figure.add_subplot(111, projection="3d")
    origins = [(0.0, 0.0, 0.0), wearable]
    all_points = np.array(origins)
    center = all_points.mean(axis=0)
    radius = max(25.0, float(np.max(np.abs(all_points - center))) + 15)
    for axis, value in zip((ax.set_xlim, ax.set_ylim, ax.set_zlim), center):
        axis(value - radius, value + radius)
    ax.set_box_aspect((1, 1, 1))
    ax.view_init(elev=elevation, azim=azimuth)
    ax.set(xlabel="X forward (cm)", ylabel="Y left (cm)", zlabel="Z up (cm)")
    ax.plot(*np.array(origins).T, color="gray", linestyle="--")
    for sensor, label, origin in zip(("board", "wt"), ("ESP32 / QMI8658", "Wearable / WT901"), origins):
        ax.scatter(*origin, s=90)
        ax.text(*np.add(origin, (0, 0, -4)), label)
        matrix = frame[sensor]
        if matrix is None:
            ax.text(*np.add(origin, (0, 0, 5)), "INVALID", color="red")
            continue
        for axis_index, (color, axis_label) in enumerate(zip(("red", "green", "blue"), ("X", "Y", "Z"))):
            direction = np.array(matrix)[:, axis_index] * 10
            ax.quiver(*origin, *direction, color=color, arrow_length_ratio=0.25)
            ax.text(*np.add(origin, direction), axis_label, color=color)
    arrow_origin = (center[0] - radius + 3, center[1] + radius - 5, center[2] - radius + 4)
    ax.quiver(*arrow_origin, 12, 0, 0, color="black", arrow_length_ratio=0.25)
    ax.text(*np.add(arrow_origin, (0, 0, 3)), "Bike front (+X, reference)")
    relative = frame["relative_angle_deg"]
    text = "unpaired / unavailable" if relative is None else f"relative change {relative:.1f} deg"
    ax.set_title(f"t = {frame['time_ms'] / 1000:.3f}s | {text}\nFixed illustrative mounts; RGB = sensor XYZ; initial pose zeroed")
    output = io.BytesIO()
    figure.savefig(output, format="png")
    return output.getvalue()


class ImuServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address):
        super().__init__(address, Handler)
        self.lock = threading.Lock()
        self.upload_lock = threading.Lock()
        self.dataset = None
        self.dataset_id = ""
        self.cache = OrderedDict()


class Handler(BaseHTTPRequestHandler):
    server: ImuServer

    def respond(self, status, content, mime="application/json"):
        if not isinstance(content, bytes):
            content = json.dumps(content, allow_nan=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(content)

    def do_POST(self):
        if self.path != "/api/upload":
            self.respond(404, {"error": "Not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= MAX_UPLOAD:
                self.respond(413, {"error": "Upload must be 1 byte to 64 MiB"})
                return
            self.connection.settimeout(60)
            with self.server.upload_lock:
                raw = self.rfile.read(length)
                if len(raw) != length:
                    raise ValueError("Incomplete upload")
                dataset = analyze(raw)
                with self.server.lock:
                    self.server.dataset = dataset
                    self.server.dataset_id = uuid.uuid4().hex
                    self.server.cache.clear()
                    result = {"id": self.server.dataset_id, "summary": dataset["summary"],
                              "times": [frame["time_ms"] for frame in dataset["frames"]]}
            self.respond(200, result)
        except (ValueError, UnicodeError) as error:
            self.respond(400, {"error": str(error)})
        except OSError as error:
            self.log_error("Upload IO failure: %s", error)
            self.respond(500, {"error": "Upload IO failure"})

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path in STATIC_FILES:
            relative, mime = STATIC_FILES[url.path]
            try:
                content = Path(__file__).parent.joinpath(relative).read_bytes()
            except OSError as error:
                self.log_error("Static asset unavailable: %s", error)
                self.respond(500, {"error": f"Required asset unavailable: {relative}"})
                return
            self.respond(200, content, mime)
            return
        query = parse_qs(url.query)
        try:
            with self.server.lock:
                if url.path not in ("/api/frame", "/api/analysis", "/api/playback"):
                    self.respond(404, {"error": "Not found"})
                    return
                data = self.server.dataset
                if data is None or query.get("id", [""])[0] != self.server.dataset_id:
                    self.respond(404, {"error": "Dataset missing or replaced; upload again"})
                    return
                if url.path in ("/api/analysis", "/api/playback"):
                    response = data
                    if url.path == "/api/playback":
                        response = {"summary": data["summary"], "frames": [
                            {key: value for key, value in frame.items() if key != "values"}
                            for frame in data["frames"]
                        ]}
                    payload = json.dumps(response, allow_nan=False).encode()
                    mime = "application/json"
                else:
                    index = int(query.get("index", ["0"])[0])
                    elevation = float(query.get("elev", ["25"])[0])
                    azimuth = float(query.get("azim", ["-60"])[0])
                    wearable = tuple(float(query.get(key, [default])[0]) for key, default in (
                        ("x", "15"), ("y", "-10"), ("z", "0")))
                    if not 0 <= index < len(data["frames"]):
                        raise ValueError("Frame index out of range")
                    if not all(math.isfinite(value) and abs(value) <= 360 for value in (elevation, azimuth, *wearable)):
                        raise ValueError("Camera and mounting values must be finite and within +/-360")
                    key = (index, elevation, azimuth, wearable)
                    if key not in self.server.cache:
                        self.server.cache[key] = render_frame(data, index, elevation, azimuth, wearable)
                        if len(self.server.cache) > 32:
                            self.server.cache.popitem(last=False)
                    payload = self.server.cache[key]
                    mime = "image/png"
            self.respond(200, payload, mime)
        except (ValueError, IndexError) as error:
            self.respond(400, {"error": str(error)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    with ImuServer(("127.0.0.1", args.port)) as server:
        print(f"IMU playback: http://127.0.0.1:{server.server_port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
