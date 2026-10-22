import os
import shlex
import subprocess
from pathlib import Path

import pytest
from pytest_embedded import Dut
from pytest_embedded.log import MessageQueue


ROOT = Path(__file__).resolve().parents[2]
SUITES = {
    "concurrency": (
        ["concurrency_test.cpp"],
        ["gps_time.cpp", "runtime_log.cpp"],
        ["PASS UI:", "PASS GPS:", "PASS SD:"],
    ),
    "fit_export": (
        ["fit_export_test.cpp"],
        ["fit_export.cpp", "fit_activity.cpp", "runtime_log.cpp"],
        ["PASS FIT export:"],
    ),
    "fit_activity": (
        ["fit_activity_test.cpp"],
        ["fit_activity.cpp"],
        ["PASS FIT:"],
    ),
    "ble_runtime": (
        ["ble_diagnostics_test.cpp"],
        [
            "runtime_log.cpp", "ble_diagnostics.cpp", "ble_scan.cpp",
            "ble_power.cpp", "ble_posture.cpp", "ble_manager.cpp",
        ],
        ["PASS BLE scanner:", "PASS BLE inbox:", "PASS BLE manager/data:",
         "PASS BLE diagnostics:"],
    ),
}


@pytest.mark.host
@pytest.mark.parametrize("suite", SUITES)
def test_native_regression(suite: str, tmp_path: Path, dut: Dut, msg_queue: MessageQueue) -> None:
    tests, sources, markers = SUITES[suite]
    binary = tmp_path / suite
    command = shlex.split(os.environ.get("CXX", "c++"))
    command += ["-std=c++17", "-pthread", "-DARDUINO=10800",
                "-Itest/host/mocks", "-Isrc"]
    command += [str(Path("test/host") / name) for name in tests]
    command += [str(Path("src") / name) for name in sources]
    if suite == "concurrency":
        gps = Path(os.environ.get(
            "GPS_SOURCE_DIR", str(ROOT / ".pio/libdeps/esp32s3/TinyGPSPlus/src")
        ))
        assert (gps / "TinyGPS++.cpp").is_file(), "Set GPS_SOURCE_DIR to TinyGPSPlus/src"
        command += ["-I" + str(gps), str(gps / "TinyGPS++.cpp")]
    command += ["-o", str(binary)]
    built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert built.returncode == 0, built.stdout + built.stderr
    result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, timeout=120)
    output = result.stdout + result.stderr
    msg_queue.put(output)
    assert result.returncode == 0, output.decode(errors="replace")
    dut.expect_exact(markers, expect_all=True, timeout=10)