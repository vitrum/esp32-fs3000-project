import csv
from http.client import HTTPConnection
import io
import json
import math
import threading
import unittest
import struct
from datetime import datetime, timezone

import numpy as np

from tools.imu_web.server import ImuServer, analyze, render_frame, rotation
from imu_to_bvh_hdf5 import REQUIRED_COLUMNS
from tools.imu_web.activity import decode_activity
from tools.imu_web.mount_analysis import diagnose_mount


def fixture(count=11):
    rows = []
    for index in range(count):
        time = 1000 + index * 100
        row = dict.fromkeys(REQUIRED_COLUMNS, "0")
        row.update(t_ms=str(time), board_t_ms=str(time), wt_t_ms=str(time),
                   board_seq=str(index), wt_seq=str(index),
                   board_valid="1", wt_valid="1", paired="1",
                   board_az_g="1", wt_az_g="1", wt_gz_dps="90")
        rows.append(row)
    return rows


def encode(rows):
    output = io.StringIO()
    writer = csv.DictWriter(output, fieldnames=REQUIRED_COLUMNS)
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue().encode()


def fit_fixture(records, wind_unit="m/s", developer=True):
    """Small external FIT producer for controlled parser inputs, not a decoder."""
    data = bytearray()
    def definition(local, global_number, fields, extra=()):
        data.extend(struct.pack("<BBBH", 0x40 | local | (0x20 if extra else 0), 0, 0, global_number))
        data.append(len(fields))
        for field in fields:
            data.extend(bytes(field))
        if extra:
            data.append(len(extra))
            for field in extra:
                data.extend(bytes(field))
    if developer:
        definition(0, 207, [(1, 16, 0x0D), (3, 1, 2)])
        data.extend(b"\0AeroProbeFIT\0\0\0\1\0")
        definition(1, 206, [(0, 1, 2), (1, 1, 2), (2, 1, 2), (3, 24, 7), (8, 8, 7)])
        for number, name, unit, base_type in ((0, "airspeed", wind_unit, 0x88), (1, "power_source", "", 2)):
            data.extend(bytes((1, 0, number, base_type)))
            data.extend(name.encode().ljust(24, b"\0"))
            data.extend(unit.encode().ljust(8, b"\0"))
    definition(2, 20, [(253, 4, 0x86), (7, 2, 0x84), (4, 1, 2), (6, 2, 0x84)],
               [(0, 4, 0), (1, 1, 0)] if developer else ())
    epoch = int(datetime(1989, 12, 31, tzinfo=timezone.utc).timestamp())
    for unix_seconds, power, cadence, wind, source in records:
        data.extend(struct.pack("<BIHBH", 2, unix_seconds - epoch,
                                0xFFFF if power is None else power,
                                0xFF if cadence is None else cadence, 10000))
        if developer:
            data.extend(struct.pack("<fB", float("nan") if wind is None else wind, source))
    def crc(raw):
        result = 0
        for byte in raw:
            result ^= byte
            for _ in range(8):
                result = (result >> 1) ^ 0xA001 if result & 1 else result >> 1
        return result
    header = struct.pack("<BBHI4s", 14, 0x20, 2100, len(data), b".FIT")
    payload = header + struct.pack("<H", crc(header)) + data
    return payload + struct.pack("<H", crc(payload))


class AnalysisTests(unittest.TestCase):
    def test_actual_gyro_motion_relative_rotation_and_png(self):
        data = analyze(encode(fixture()))
        self.assertEqual(data["summary"]["rows"], 11)
        self.assertEqual([frame["time_ms"] for frame in data["frames"]], list(range(0, 1001, 100)))
        self.assertAlmostEqual(data["frames"][-1]["relative_angle_deg"], 90, places=6)
        np.testing.assert_allclose(data["frames"][-1]["board"], np.eye(3), atol=1e-10)
        np.testing.assert_allclose(np.array(data["frames"][-1]["wt"])[:, 0], [0, 1, 0], atol=1e-10)
        from PIL import Image
        first = render_frame(data, 0)
        last = render_frame(data, 10)
        self.assertNotEqual(first, last)
        image = Image.open(io.BytesIO(last))
        self.assertEqual(image.format, "PNG")
        self.assertEqual(image.size, (855, 570))
        self.assertGreater(np.asarray(image).std(), 10)

    def test_invalid_sensor_and_zero_acceleration_are_visible_not_fabricated(self):
        rows = fixture()
        rows[3]["board_az_g"] = "0"
        rows[5].update(wt_valid="0", paired="0", wt_az_g="nan")
        result = analyze(encode(rows))
        self.assertEqual(result["summary"]["low_acceleration_rejected"], {"board": 1, "wt": 0})
        self.assertEqual(result["summary"]["paired_rows"], 9)
        self.assertIsNone(result["frames"][3]["board"])
        self.assertIsNone(result["frames"][3]["relative_angle_deg"])
        self.assertIsNone(result["frames"][5]["wt"])
        self.assertIsNone(result["frames"][5]["values"]["wt_az_g"])
        json.dumps(result, allow_nan=False)

    def test_missing_columns_stale_pair_and_no_usable_acceleration_fail(self):
        with self.assertRaisesRegex(ValueError, "missing required columns"):
            analyze(b"t_ms\n0\n")
        rows = fixture()
        rows[1]["wt_t_ms"] = "0"
        with self.assertRaisesRegex(ValueError, "stale or inconsistent"):
            analyze(encode(rows))
        rows = fixture()
        for row in rows:
            row["wt_az_g"] = "0"
        with self.assertRaisesRegex(ValueError, "No synchronized pairs"):
            analyze(encode(rows))

    def test_rotation_convention_and_uint32_wrap(self):
        np.testing.assert_allclose(rotation((0, 0, 90)) @ [1, 0, 0], [0, 1, 0], atol=1e-10)
        rows = fixture(2)
        for row, time in zip(rows, (0xFFFFFFF0, 84)):
            for key in ("t_ms", "board_t_ms", "wt_t_ms"):
                row[key] = str(time)
        result = analyze(encode(rows))
        self.assertEqual(result["frames"][1]["time_ms"], 100)

    def test_inverted_board_and_upright_arm_share_same_world_turn(self):
        rows = fixture()
        for row in rows:
            row.update(board_az_g="-1", board_gz_dps="-90")
        result = analyze(encode(rows))
        last = result["frames"][-1]
        self.assertAlmostEqual(last["board_heading_change_deg"], 90, places=6)
        self.assertAlmostEqual(last["wt_heading_change_deg"], 90, places=6)
        np.testing.assert_allclose(last["board"], last["wt"], atol=1e-10)
        self.assertAlmostEqual(last["relative_angle_deg"], 0, places=5)

    def test_heading_ignores_sensor_time_rewind_without_losing_next_interval(self):
        rows = fixture()
        for row in rows:
            row["board_gz_dps"] = "90"
        rows[3].update(paired="0", board_t_ms="100")
        result = analyze(encode(rows))
        self.assertAlmostEqual(result["frames"][-1]["board_heading_change_deg"], 90, places=6)

    def test_vertical_native_x_yaw_is_not_counted_as_forearm_roll(self):
        rows = fixture()
        for row in rows:
            row.update(board_az_g="-1", board_gz_dps="-90", wt_ax_g="-1",
                       wt_az_g="0", wt_gx_dps="-90", wt_gz_dps="0")
        data = analyze(encode(rows))
        np.testing.assert_allclose(data["frames"][-1]["board"], data["frames"][-1]["wt"], atol=1e-10)
        self.assertAlmostEqual(data["frames"][-1]["relative_angle_deg"], 0, places=5)
        self.assertEqual(data["frames"][-1]["values"]["wt_gx_dps"], -90)

    def test_mount_analysis_preserves_data_and_requires_quiet_hood_reference(self):
        rows = fixture(30)
        for index, row in enumerate(rows):
            row.update(wt_ax_g="-1", wt_az_g="0", wt_gz_dps="0",
                       wt_gx_dps="0" if index < 10 else "-45",
                       board_gz_dps="0" if index < 10 else "45")
        data = analyze(encode(rows))
        original = json.dumps(data["frames"])
        config = {"mode": "hood", "shoulder": [-27, -18, 32], "grip": [4, -19, 1.5],
                  "neutral_direction": [1, 0, 0], "forearm_and_palm": 30, "upper_arm": 31.62}
        result = diagnose_mount(data, config)
        self.assertEqual(json.dumps(data["frames"]), original)
        self.assertEqual(result["baseline"]["samples"], 30)
        candidate = result["candidates"][0]
        alignment, native = np.array(candidate["alignment"]), np.array(candidate["native_from_arm"])
        np.testing.assert_allclose(alignment @ [0, 0, 1], [0, 0, 1], atol=1e-10)
        self.assertAlmostEqual(np.linalg.det(native), 1)
        reference = np.array(candidate["sensor_reference"])
        first = alignment @ np.array(data["frames"][0]["wt"]) @ reference @ native
        last = alignment @ np.array(data["frames"][-1]["wt"]) @ reference @ native
        np.testing.assert_allclose(last, np.array(data["frames"][-1]["board"]) @ first, atol=1e-9)
        with self.assertRaisesRegex(ValueError, "supported"):
            diagnose_mount(data, {**config, "mode": "free"})
        with self.assertRaisesRegex(ValueError, "quiet"):
            diagnose_mount(analyze(encode(fixture(10))), config)
        quiet_rows = fixture(10)
        for row in quiet_rows:
            row.update(board_gz_dps="0", wt_gz_dps="0", wt_ax_g=str(-math.sin(math.radians(75))),
                       wt_ay_g="0", wt_az_g=str(math.cos(math.radians(75))))
        equal_ratios = {**config, "shoulder": config["grip"], "upper_arm": 30}
        ranked = diagnose_mount(analyze(encode(quiet_rows)), equal_ratios)
        best = ranked["candidates"][0]
        self.assertLess(best["reference_direction_error_deg"], 1e-6)
        self.assertAlmostEqual(abs(best["native_from_arm"][1][0]), 1)
        for row in quiet_rows[1:]:
            row.update(board_ax_g=str(-math.sin(math.radians(45))),
                       board_az_g=str(math.cos(math.radians(45))))
        tilting_board = analyze(encode(quiet_rows))
        self.assertGreater(abs(tilting_board["frames"][-1]["board"][2][0]), 0.1)
        grounded = diagnose_mount(tilting_board, config)
        self.assertEqual(grounded["baseline"]["outliers"], 0)

    def test_signed_display_swaps_reach_diagnosis_without_modifying_raw_data(self):
        rows = fixture(10)
        for row in rows:
            row.update(board_gz_dps="0", wt_gz_dps="0")
        data = analyze(encode(rows))
        original = json.dumps(data["frames"])
        config = {"mode": "hood", "shoulder": [-27, -18, 32], "grip": [4, -19, 1.5],
                  "neutral_direction": [1, 0, 0], "forearm_and_palm": 30, "upper_arm": 31.62}
        for matrix in (
            [[0, -1, 0], [1, 0, 0], [0, 0, 1]],
            [[0, 1, 0], [-1, 0, 0], [0, 0, 1]],
            [[1, 0, 0], [0, 0, -1], [0, 1, 0]],
            [[1, 0, 0], [0, 0, 1], [0, -1, 0]],
            [[0, 0, -1], [0, 1, 0], [1, 0, 0]],
            [[0, 0, 1], [0, 1, 0], [-1, 0, 0]],
        ):
            with self.subTest(matrix=matrix):
                result = diagnose_mount(data, {**config, "display_mount": matrix})
                self.assertEqual(result["model_reference"]["display_mount"], matrix)
                direction = np.array(matrix)[:, 0]
                expected = np.linalg.norm(np.array(config["grip"]) - 30 * direction
                                          - config["shoulder"]) / config["upper_arm"]
                self.assertAlmostEqual(result["baseline"]["ratio_p05"], expected)
                self.assertAlmostEqual(result["baseline"]["ratio_p95"], expected)
                self.assertEqual(json.dumps(data["frames"]), original)
        with self.assertRaisesRegex(ValueError, "proper rotation"):
            diagnose_mount(data, {**config, "display_mount": [[0, 1, 0], [1, 0, 0], [0, 0, 1]]})


class FitTests(unittest.TestCase):
    def test_utc_units_missing_source_and_valid_zero_are_preserved(self):
        start = 1792713540
        data = decode_activity(fit_fixture([
            (start, 0, 0, 0, 1), (start + 1, 300, 90, 36, 1),
            (start + 2, None, None, None, 0),
        ], wind_unit="km/h"))
        self.assertEqual(data["records"][0]["power_w"], 0)
        self.assertEqual(data["records"][0]["cadence_rpm"], 0)
        self.assertEqual(data["records"][0]["wind_mps"], 0)
        self.assertEqual(data["records"][0]["utc_ms"], start * 1000)
        self.assertAlmostEqual(data["records"][1]["wind_mps"], 10)
        self.assertIsNone(data["records"][2]["power_w"])
        self.assertIsNone(data["records"][2]["wind_mps"])
        generic = decode_activity(fit_fixture([(start, 300, 90, 10, 1)], developer=False))
        self.assertEqual(generic["records"][0]["speed_mps"], 10)
        self.assertIsNone(generic["records"][0]["wind_mps"])
        self.assertTrue(generic["summary"]["warnings"])

    def test_crc_truncation_unknown_units_and_rewound_time(self):
        valid = fit_fixture([(1792713540, 300, 90, 10, 1)])
        for invalid in (b"not a FIT", valid[:-1], valid[:-1] + bytes([valid[-1] ^ 1])):
            with self.assertRaisesRegex(ValueError, "FIT"):
                decode_activity(invalid)
        unknown = decode_activity(fit_fixture([(1792713540, 300, 90, 10, 1)], wind_unit="knots"))
        self.assertIsNone(unknown["records"][0]["wind_mps"])
        with self.assertRaisesRegex(ValueError, "backwards"):
            decode_activity(fit_fixture([(1792713541, 300, 90, 10, 1), (1792713540, 300, 90, 10, 1)]))


class HttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ImuServer(("127.0.0.1", 0))
        cls.thread = threading.Thread(target=cls.server.serve_forever)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.thread.join(timeout=5)
        cls.server.server_close()

    def request(self, method, path, body=None, headers=None):
        connection = HTTPConnection("127.0.0.1", self.server.server_port, timeout=30)
        try:
            connection.request(method, path, body, headers or {})
            response = connection.getresponse()
            return response.status, response.getheader("Content-Type"), response.read()
        finally:
            connection.close()

    def test_upload_render_download_errors_and_dataset_replacement(self):
        status, _, page = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b'id="timeline"', page)
        status, _, raw = self.request("POST", "/api/upload", encode(fixture()))
        self.assertEqual(status, 200)
        result = json.loads(raw)
        dataset_id = result["id"]
        self.assertEqual(len(result["times"]), 11)
        status, _, raw = self.request("GET", f"/api/playback?id={dataset_id}")
        self.assertEqual(status, 200)
        playback = json.loads(raw)
        self.assertEqual(len(playback["frames"]), 11)
        self.assertNotIn("values", playback["frames"][0])
        np.testing.assert_allclose(playback["frames"][-1]["wt"],
                                   [[0,-1,0],[1,0,0],[0,0,1]], atol=1e-10)
        status, mime, png = self.request("GET", f"/api/frame?id={dataset_id}&index=10")
        self.assertEqual((status, mime), (200, "image/png"))
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        status, _, raw = self.request("GET", f"/api/analysis?id={dataset_id}")
        self.assertEqual(status, 200)
        self.assertAlmostEqual(json.loads(raw)["frames"][-1]["relative_angle_deg"], 90)
        for suffix in ("index=11", "elev=nan", "x=1000", "index=bad"):
            self.assertEqual(self.request("GET", f"/api/frame?id={dataset_id}&{suffix}")[0], 400)
        self.assertEqual(self.request("POST", "/api/upload", b"bad")[0], 400)
        self.assertEqual(self.request("GET", f"/api/frame?id={dataset_id}")[0], 200)
        self.assertEqual(self.request("POST", "/api/upload", headers={"Content-Length": "67108865"})[0], 413)
        self.request("POST", "/api/upload", encode(fixture(2)))
        self.assertEqual(self.request("GET", f"/api/frame?id={dataset_id}")[0], 404)
        self.assertEqual(self.request("GET", f"/api/playback?id={dataset_id}")[0], 404)

    def test_local_three_modules_and_safe_static_routing(self):
        for path in ("/viewer.js", "/scene.js", "/bike_model.js", "/activity.js", "/aero.js", "/test_scene.js",
                     "/vendor/three.module.min.js", "/vendor/three.core.min.js", "/vendor/OrbitControls.js"):
            status, mime, body = self.request("GET", path)
            self.assertEqual(status, 200, path)
            self.assertIn("javascript", mime)
            self.assertGreater(len(body), 100)
        for path in ("/server.py", "/vendor/../server.py", "/vendor/LICENSE"):
            self.assertEqual(self.request("GET", path)[0], 404)

    def test_fit_upload_is_independent_and_failed_upload_preserves_activity(self):
        status, _, raw = self.request("POST", "/api/fit-upload", fit_fixture([(1792713540, 300, 90, 10, 1)]))
        self.assertEqual(status, 200, raw)
        activity_id = json.loads(raw)["id"]
        self.request("POST", "/api/upload", encode(fixture()))
        status, _, raw = self.request("GET", f"/api/activity?id={activity_id}")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(raw)["records"][0]["power_w"], 300)
        self.assertEqual(self.request("POST", "/api/fit-upload", b"bad")[0], 400)
        self.assertEqual(self.request("GET", f"/api/activity?id={activity_id}")[0], 200)
        self.assertEqual(self.request("POST", "/api/mount?id=missing", b"{}")[0], 404)


if __name__ == "__main__":
    unittest.main()
