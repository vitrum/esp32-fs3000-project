import csv
from http.client import HTTPConnection
import io
import json
import threading
import unittest

import numpy as np

from tools.imu_web.server import ImuServer, analyze, render_frame, rotation
from imu_to_bvh_hdf5 import REQUIRED_COLUMNS


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
        for path in ("/viewer.js", "/scene.js", "/bike_model.js", "/test_scene.js",
                     "/vendor/three.module.min.js", "/vendor/three.core.min.js", "/vendor/OrbitControls.js"):
            status, mime, body = self.request("GET", path)
            self.assertEqual(status, 200, path)
            self.assertIn("javascript", mime)
            self.assertGreater(len(body), 100)
        for path in ("/server.py", "/vendor/../server.py", "/vendor/LICENSE"):
            self.assertEqual(self.request("GET", path)[0], 404)


if __name__ == "__main__":
    unittest.main()
