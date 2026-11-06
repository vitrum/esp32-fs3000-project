import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import flash_app


class FlashSafetyTests(unittest.TestCase):
    def test_partition_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "partitions.csv"
            path.write_text("# Name, Type, SubType, Offset, Size\nnvs,data,nvs,0x9000,0x5000\napp0, app, ota_0, 0x10000, 0x300000\n")
            self.assertEqual(flash_app.app_partition(path), (0x10000, 0x300000))
            path.write_text("app0,app,ota_0,0x20000,0x300000\n")
            with self.assertRaises(ValueError):
                flash_app.app_partition(path)

    def test_chunks_and_erase_bounds(self):
        plan = flash_app.chunk_plan(1804608, 0x10000, 0x300000)
        self.assertEqual(sum(length for _, _, length in plan), 1804608)
        self.assertEqual(plan[0][0], 0x10000)
        for address, _, length in plan:
            self.assertEqual(address % 4096, 0)
            self.assertLessEqual(length, 96 * 1024)
            self.assertLessEqual(address + length, 0x310000)
        for size in (0, -1, 0x300001):
            with self.assertRaises(ValueError):
                flash_app.chunk_plan(size, 0x10000, 0x300000)

    def test_identity_and_port_change(self):
        board = SimpleNamespace(vid=0x303A, pid=0x1001, serial_number="D8:3B:DA:9B:6F:68", device="/dev/cu.new")
        with patch.object(flash_app.list_ports, "comports", return_value=[board]):
            self.assertEqual(flash_app.rom_port("d83bda9b6f68"), "/dev/cu.new")
            with patch.object(flash_app.select, "kqueue", create=True) as events:
                events.return_value.control.return_value = []
                with self.assertRaises(ValueError):
                    flash_app.rom_port("001122334455")
            board.serial_number = "D83BDA9B6F68"
            with self.assertRaises(ValueError):
                flash_app.rom_port("d83bda9b6f68")

    def test_resume_only_at_valid_boundary(self):
        plan = flash_app.chunk_plan(1804608, 0x10000, 0x300000)
        remaining = flash_app.remaining_plan(plan, 0x58000)
        self.assertEqual(remaining[0], (0x58000, 3 * 96 * 1024, 96 * 1024))
        self.assertEqual(len(plan) - len(remaining), 3)
        for invalid in (0x8000, 0x10001, 0x400000):
            with self.assertRaises(ValueError):
                flash_app.remaining_plan(plan, invalid)

    def test_no_false_success(self):
        identity = "MAC: d8:3b:da:9b:6f:68\n"
        marker = "Hash of data verified"
        self.assertTrue(flash_app.successful(0, identity + marker, marker, "d83bda9b6f68"))
        self.assertFalse(flash_app.successful(1, identity + marker, marker, "d83bda9b6f68"))
        self.assertFalse(flash_app.successful(0, marker, marker, "d83bda9b6f68"))
        self.assertFalse(flash_app.successful(0, identity + marker, marker, "001122334455"))


if __name__ == "__main__":
    unittest.main()