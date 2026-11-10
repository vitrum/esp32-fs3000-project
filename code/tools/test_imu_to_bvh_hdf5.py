import csv
import tempfile
import unittest
from pathlib import Path

import h5py

from tools.imu_to_bvh_hdf5 import (
    BVH_SKELETON,
    REQUIRED_COLUMNS,
    _bvh_channel_names,
    _bvh_joint_names,
    bvh_channel_rows,
    convert,
    load_csv,
)


def make_row(time_ms, paired=True, skew_ms=10, angle=0.0):
    row = {
        "t_ms": str(time_ms & 0xFFFFFFFF),
        "board_t_ms": str((time_ms - 10) & 0xFFFFFFFF),
        "wt_t_ms": str((time_ms - 20 - (skew_ms - 10)) & 0xFFFFFFFF),
        "board_seq": str(time_ms & 0xFFFFFFFF),
        "wt_seq": str((time_ms + 1) & 0xFFFFFFFF),
        "board_valid": "1",
        "wt_valid": "1",
        "paired": "1" if paired else "0",
        "skew_ms": str(skew_ms),
        "wt_roll_deg": str(angle),
        "wt_pitch_deg": str(angle / 2),
        "wt_yaw_deg": str(angle / 4),
    }
    for sensor in ("board", "wt"):
        row.update({
            f"{sensor}_ax_g": str(-angle / 180),
            f"{sensor}_ay_g": "0",
            f"{sensor}_az_g": "1",
            f"{sensor}_gx_dps": "0",
            f"{sensor}_gy_dps": "0",
            f"{sensor}_gz_dps": "1",
        })
    return row


class ImuConversionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def write_csv(self, rows):
        path = self.root / "sample.csv"
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=REQUIRED_COLUMNS)
            writer.writeheader()
            writer.writerows(rows)
        return path

    def test_converts_valid_pairs_and_preserves_every_source_row(self):
        rows = [
            make_row(1000, angle=0),
            make_row(1100, angle=10),
            make_row(1200, paired=False, skew_ms=120, angle=20),
            make_row(1300, angle=30),
            make_row(1400, angle=40),
        ]
        source = self.write_csv(rows)
        bvh, hdf5, row_count, pair_count = convert(source, self.root / "converted")

        self.assertEqual((row_count, pair_count), (5, 4))
        text = bvh.read_text(encoding="utf-8")
        self.assertIn("ROOT Ground", text)
        self.assertIn("OFFSET 0.00 0.00 0.00", text)
        self.assertIn("JOINT Hips", text)
        self.assertIn("OFFSET 0.00 95.00 0.00", text)
        for joint in (
            "LeftHip", "LeftKnee", "RightKnee", "Spine", "Chest", "Neck", "Head",
            "LeftUpperArm", "LeftForearm", "RightUpperArm", "RightElbow",
            "RightForearm", "RightHand",
        ):
            self.assertIn(f"JOINT {joint}", text)
        for removed in ("BikeFrame", "RearWheel", "HeadTube", "Handlebar", "FrontFork"):
            self.assertNotIn(removed, text)
        self.assertIn("Frames: 5", text)
        self.assertIn("Frame Time: 0.100000000", text)
        motion_rows = text.split("Frame Time: ")[1].strip().splitlines()[1:]
        self.assertEqual(len(motion_rows), 5)
        expected_channel_count = 6 + 3 * text.count("CHANNELS 3")
        self.assertEqual(len(motion_rows[0].split()), expected_channel_count)
        self.assertEqual(expected_channel_count, len(_bvh_channel_names()))
        self.assertEqual(len(bvh_channel_rows([(0, 0, 0, 0, 0, 0)])[0]),
                         expected_channel_count)
        self.assertEqual(len(set(_bvh_joint_names())), len(_bvh_joint_names()))

        offsets = []

        def collect_offsets(joints, parent_y=0):
            for name, (_, y, _), _, children in joints:
                joint_y = parent_y + y
                offsets.append(joint_y)
                collect_offsets(children, joint_y)

        collect_offsets(BVH_SKELETON, parent_y=95)
        self.assertEqual(min(offsets), 0)
        self.assertGreater(max(offsets), 160)
        self.assertLess(max(offsets), 180)

        with h5py.File(hdf5, "r") as file:
            self.assertEqual(file.attrs["coordinate_calibration"], "unknown; sensor-native axes")
            self.assertIn("human-only illustrative cycling pose", file.attrs["bvh_interpretation"])
            self.assertEqual(file["samples/t_ms"].shape, (5,))
            self.assertEqual(file["samples/accepted_pair"][:].tolist(),
                             [True, True, False, True, True])
            self.assertEqual(file["samples/board_acceleration_g"].shape, (5, 3))
            self.assertEqual(file["samples/wearable_reported_euler_deg"].shape, (5, 3))
            self.assertEqual(file["bvh/frame_euler_deg"].shape, (5, 6))
            self.assertEqual(file["bvh/frame_channels"].shape,
                             (5, expected_channel_count))
            channel_names = file["bvh/frame_channels"].attrs["channel_names"].split(",")
            self.assertEqual(channel_names, _bvh_channel_names())
            self.assertEqual(file["bvh/frame_channels"].attrs["channel_units"].split(",")[:3],
                             ["cm", "cm", "cm"])
            self.assertEqual(file["bvh"].attrs["joint_names"].split(","),
                             _bvh_joint_names())
            self.assertIn("cm", file["bvh"].attrs["offset_units"])
            self.assertEqual(file["raw/source_csv_utf8"][:].tobytes(), source.read_bytes())

    def test_maps_sensor_angles_to_declared_bvh_channels(self):
        names = _bvh_channel_names()
        values = bvh_channel_rows([(1, 2, 3, 7, 8, 9)])[0]
        channels = dict(zip(names, values))

        self.assertEqual(
            [channels[f"Ground.{axis}"] for axis in ("Xposition", "Yposition", "Zposition",
                                                      "Zrotation", "Xrotation", "Yrotation")],
            [0, 0, 0, 0, 0, 0],
        )
        self.assertEqual(
            [channels[f"Hips.{axis}"] for axis in ("Zrotation", "Xrotation", "Yrotation")],
            [0, 0, 0],
        )
        self.assertEqual(
            [channels[f"Spine.{axis}"] for axis in ("Zrotation", "Xrotation", "Yrotation")],
            [-35, 0, 0],
        )
        self.assertEqual(
            [channels[f"RightForearm.{axis}"] for axis in ("Zrotation", "Xrotation", "Yrotation")],
            [6, 6, 0],
        )
        self.assertTrue(all(value == 0 for name, value in channels.items()
                            if name.startswith(("Left", "RightHip", "RightKnee", "RightAnkle",
                                                "RightFoot", "RightToe", "RightUpperArm",
                                                "RightElbow", "Left"))))

    def test_rejects_pair_with_mismatched_timestamps(self):
        row = make_row(1000, skew_ms=10)
        row["skew_ms"] = "11"
        source = self.write_csv([row])
        with self.assertRaisesRegex(ValueError, "timestamps are stale or inconsistent"):
            load_csv(source)

    def test_unwraps_millis_counter_rollover(self):
        rows = [
            make_row(0xFFFFFF00, angle=0),
            make_row(0xFFFFFF64, angle=10),
            make_row(0xFFFFFFC8, angle=20),
            make_row(0x0000002C, angle=30),
        ]
        source = self.write_csv(rows)
        _, samples, _ = load_csv(source)
        self.assertEqual([sample.elapsed_ms for sample in samples], [0, 100, 200, 300])

    def test_refuses_existing_outputs_without_force(self):
        source = self.write_csv([make_row(1000), make_row(1100, angle=10)])
        prefix = self.root / "converted"
        bvh, _, _, _ = convert(source, prefix)
        bvh_before = bvh.read_bytes()
        with self.assertRaisesRegex(FileExistsError, "output exists"):
            convert(source, prefix)
        self.assertEqual(bvh.read_bytes(), bvh_before)


if __name__ == "__main__":
    unittest.main()
