import csv
import hashlib
import io
import json
import os
import shlex
import struct
import subprocess
import tempfile
from collections import Counter
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest
import fitdecode
from pytest_embedded import Dut
from pytest_embedded.log import MessageQueue


ROOT = Path(__file__).resolve().parents[2]
SUITES = {
    "ui_language": (
        ["ui_language_test.cpp"], [], ["PASS UI language:"],
    ),
    "sensors_power": (
        ["sensors_power_test.cpp"],
        ["fs3000.cpp", "wind_probe.cpp", "bme280_env.cpp", "power_key.cpp", "runtime_log.cpp", "audio.cpp"],
        ["PASS FS3000:", "PASS wind:", "PASS environment:", "PASS power key:", "PASS audio disabled:"],
    ),
    "system_coexistence": (
        ["system_coexistence_test.cpp"],
        ["gps_time.cpp", "runtime_log.cpp", "ble_scan.cpp",
         "ble_power.cpp", "ble_posture.cpp", "ble_heart_rate.cpp", "ble_manager.cpp", "wifi_web.cpp", "csv_logger.cpp",
         "fit_export.cpp", "fit_activity.cpp", "fs3000.cpp"],
        ["PASS coexistence:"],
    ),
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
            "runtime_log.cpp", "ble_scan.cpp",
            "ble_power.cpp", "ble_posture.cpp", "ble_heart_rate.cpp", "ble_manager.cpp",
        ],
        ["PASS BLE scanner:", "PASS BLE inbox:", "PASS BLE manager/data:",
         "PASS BLE runtime:"],
    ),
}
for scenario, marker in {"wifi": "PASS WiFi:", "storage": "PASS CSV storage:",
                         "usb_storage": "PASS USB ownership:",
                         "fit": "PASS recording FIT:", "wind_power_fit": "PASS recording FIT:",
                         "stage1_ble_wind_fit": "PASS recording FIT:",
                         "stage2_gps_ble_wind_fit": "PASS recording FIT:"}.items():
    SUITES["system_" + scenario] = (SUITES["system_coexistence"][0],
                                  SUITES["system_coexistence"][1], [marker])

SUITES["system_recording_lifecycle"] = (SUITES["system_coexistence"][0],
    SUITES["system_coexistence"][1], ["PASS recording lifecycle:"])


def validate_recovered_fit(artifacts: Path) -> None:
    reports = []
    cases = {
        "recovered": (1792483200, (10000, 11000, 14000, 15000),
                      (0, 250, 300, 400), (0, 10, 12, 15), (0, 8, 10, 11), (80, 81, 82, 83), 41.5),
        "shutdown": (1792483300, (20000, 21000), (250, 300), (10, 12), (8, 10), (90, 90), 9.0),
        "batch": (1792483400, tuple(60000 + index * 1000 for index in range(65)),
              tuple(200 + index for index in range(65)), (10,) * 65, (8,) * 65, (90,) * 65, 512.0),
    }
    for name, (epoch, times, powers, winds, speeds, cadences, distance) in cases.items():
        path = artifacts / (name + ".fit")
        data = path.read_bytes()
        with fitdecode.FitReader(path, check_crc=fitdecode.CrcCheck.RAISE,
                                 error_handling=fitdecode.ErrorHandling.RAISE) as reader:
            messages = [frame for frame in reader if isinstance(frame, fitdecode.FitDataMessage)]
        records = [message for message in messages if message.name == "record"]
        assert Counter(message.name for message in messages) == {
            "file_id": 1, "developer_data_id": 1, "field_description": 5,
            "record": len(times), "event": 2, "lap": 1, "session": 1, "activity": 1,
        }
        with (artifacts / (name + ".csv")).open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        assert len(records) == len(rows) == len(times)
        start = datetime.fromtimestamp(epoch, timezone.utc)
        total = 0.0
        for index, (record, row) in enumerate(zip(records, rows)):
            assert int(row["t_ms"]) == times[index]
            assert float(row["powerW"]) == powers[index]
            assert float(row["vAirMps"]) == winds[index]
            assert float(row["speedMps"]) == speeds[index]
            assert float(row["cadenceRpm"]) == cadences[index]
            assert int(row["powerSrc"]) == 1
            if index:
                total += (speeds[index - 1] + speeds[index]) * 0.5 * (times[index] - times[index - 1]) / 1000
            assert record.get_value("timestamp") == start + timedelta(milliseconds=times[index] - times[0])
            assert record.get_value("power") == powers[index]
            assert record.get_value("cadence") == cadences[index]
            assert record.get_value("speed") == pytest.approx(speeds[index], abs=0.0005)
            assert record.get_value("distance") == pytest.approx(total, abs=0.0051)
            for field, expected, units in (("airspeed", winds[index], "m/s"), ("yaw", 0, "deg"),
                    ("posture", -15, "deg"), ("air_density", 1.225, "kg/m3")):
                assert record.get_value(field) == pytest.approx(expected, abs=0.0001)
                assert record.get_field(field).units == units
            assert record.get_value("power_source") == 1
        assert total == distance
        for summary in [message for message in messages if message.name in {"lap", "session"}]:
            assert summary.get_value("total_elapsed_time") == (times[-1] - times[0]) / 1000
            assert summary.get_value("total_distance") == pytest.approx(distance, abs=0.0051)
        assert [message.get_value("event_type") for message in messages if message.name == "event"] == ["start", "stop_all"]
        reports.append({"case": name, "records": len(records), "distance_meters": distance,
                        "sha256": hashlib.sha256(data).hexdigest(), "content": "passed", "crc": "passed"})
    (artifacts / "validation.json").write_text(json.dumps({"lifecycle": reports}, indent=2) + "\n")


def validate_recording_fit(artifacts: Path, measurements: bool = False,
                           gps: bool = False, fallback: bool = False) -> None:
    staged = gps or fallback
    path = artifacts / "recording.fit"
    data = path.read_bytes()
    assert data[0] == 14 and data[1] == 0x20 and data[8:12] == b".FIT"
    assert len(data) == data[0] + struct.unpack_from("<I", data, 4)[0] + 2
    with fitdecode.FitReader(path, check_crc=fitdecode.CrcCheck.RAISE,
                             error_handling=fitdecode.ErrorHandling.RAISE) as reader:
        frames = list(reader)
    assert sum(isinstance(frame, fitdecode.FitHeader) for frame in frames) == 1
    assert sum(isinstance(frame, fitdecode.FitCRC) for frame in frames) == 1
    messages = [frame for frame in frames if isinstance(frame, fitdecode.FitDataMessage)]
    counts = Counter(message.name for message in messages)
    assert counts == {"file_id": 1, "developer_data_id": 1, "field_description": 5,
                      "record": 65, "event": 2, "lap": 1, "session": 1, "activity": 1}
    segments = []
    for index, expected_count in enumerate((65,) if staged else (60, 5)):
        with (artifacts / f"segment-{index}.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == expected_count
        segments.extend(rows)
    start = (datetime(2024, 1, 1, tzinfo=timezone.utc) if fallback else
             datetime(2026, 10, 22, 23, 59, tzinfo=timezone.utc))
    records = [message for message in messages if message.name == "record"]
    wind_reference = (0, 0, 1, 2, 3, 4, 6, 7, 15, 15)
    raw_reference = (0, 409, 806, 1203, 1597, 1908, 2400, 2629, 3686, 4095)
    power_reference = (0, 1, 255, 256, 2000, 32767, -25, 1000)
    if measurements:
        with (artifacts / "sensor-input.csv").open(newline="") as stream:
            inputs = list(csv.DictReader(stream))
        assert len(inputs) == 65
    distance = 0.0
    previous_speed = 0.0
    for index, (record, row) in enumerate(zip(records, segments)):
        source = (0, 1, 3)[index % 3]
        expected_power = 200 + index
        expected_cadence = 80 + index % 10
        expected_wind = 10 + index * 0.05
        if measurements:
            source = 0 if index in (30, 31) else 1
            expected_power = power_reference[index % 8] if source else 0
            expected_cadence = 0
            expected_wind = wind_reference[index % 10]
            assert int(inputs[index]["index"]) == index
            assert int(inputs[index]["raw_count"]) == raw_reference[index % 10]
            assert int(inputs[index]["packet_power_w"]) == power_reference[index % 8]
            assert inputs[index]["protocol"] == ("CPS" if index < 32 else "FTMS")
        speed = float(row["speedMps"])
        assert int(row["t_ms"]) == 10000 + index * 1000
        assert float(row["powerW"]) == expected_power
        assert float(row["cadenceRpm"]) == expected_cadence
        assert int(row["powerSrc"]) == source
        expected_speed = (0 if fallback or (gps and 22 <= index <= 24) else
                  11.524 if gps else 8 + index * 0.1)
        assert speed == pytest.approx(expected_speed, abs=0.0005)
        assert float(row["vAirMps"]) == pytest.approx(expected_wind, abs=0.0005)
        if index:
            distance += (previous_speed + speed) * 0.5
        previous_speed = speed
        assert record.get_value("timestamp") == start + timedelta(seconds=index)
        assert record.get_value("speed") == pytest.approx(speed, abs=0.0005)
        assert record.get_value("distance") == pytest.approx(distance, abs=0.0051)
        assert record.get_value("power") == (expected_power if source and expected_power >= 0 else None)
        assert record.get_value("cadence") == (expected_cadence if source else None)
        assert record.get_value("power_source") == source
        for field, column, units in (
            ("airspeed", "vAirMps", "m/s"), ("yaw", "yawDeg", "deg"),
            ("posture", "postureDeg", "deg"), ("air_density", "rhoKgM3", "kg/m3"),
        ):
            assert record.get_value(field) == pytest.approx(float(row[column]), abs=0.00001)
            assert record.get_field(field).units == units
    expected_distance = 0 if fallback else 702.964 if gps else 716.8
    assert distance == pytest.approx(expected_distance)
    if gps:
        sentences = (artifacts / "gps-input.nmea").read_text().splitlines()
        assert len(sentences) == 61
        assert sum(sentence == "$GPRMC,broken*00" for sentence in sentences) == 1
    descriptions = [message.get_value("field_name") for message in messages
                    if message.name == "field_description"]
    assert set(descriptions) == {"airspeed", "yaw", "posture", "air_density", "power_source"}
    file_id = next(message for message in messages if message.name == "file_id")
    assert file_id.get_value("type") == "activity"
    assert file_id.get_value("time_created") == start
    for name in ("lap", "session"):
        summary = next(message for message in messages if message.name == name)
        assert summary.get_value("sport") == "cycling"
        assert summary.get_value("start_time") == start
        assert summary.get_value("timestamp") == start + timedelta(seconds=64)
        assert summary.get_value("total_elapsed_time") == 64
        assert summary.get_value("total_timer_time") == 64
        assert summary.get_value("total_distance") == pytest.approx(expected_distance, abs=0.0051)
    session = next(message for message in messages if message.name == "session")
    assert session.get_value("num_laps") == 1
    activity = next(message for message in messages if message.name == "activity")
    assert activity.get_value("num_sessions") == 1
    assert activity.get_value("total_timer_time") == 64
    events = [message for message in messages if message.name == "event"]
    assert [event.get_value("event_type") for event in events] == ["start", "stop_all"]
    assert [event.get_value("timestamp") for event in events] == [start, start + timedelta(seconds=64)]
    for offset in (12, len(data) - 1):
        corrupted = bytearray(data)
        corrupted[offset] ^= 1
        with pytest.raises(fitdecode.FitCRCError):
            with fitdecode.FitReader(io.BytesIO(corrupted), check_crc=fitdecode.CrcCheck.RAISE) as reader:
                list(reader)
    with pytest.raises(fitdecode.FitEOFError):
        with fitdecode.FitReader(io.BytesIO(data[:-1]), check_crc=fitdecode.CrcCheck.RAISE) as reader:
            list(reader)
    decoded = [{"name": message.name,
                "fields": [{"name": field.name, "value": field.value, "units": field.units}
                           for field in message.fields]} for message in messages]
    (artifacts / "decoded.json").write_text(json.dumps(decoded, default=str, indent=2) + "\n")
    report = {"decoder": "fitdecode 0.11.0", "file": "recording.fit",
              "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
              "records": len(records), "csv_segments": 1 if staged else 2, "start_utc": start.isoformat(),
              "elapsed_seconds": 64, "distance_meters": distance,
              "header_crc": "passed", "file_crc": "passed", "content": "passed",
              "corruption_rejected": True, "truncation_rejected": True,
              "measurement_chain": "FS3000 raw + CPS/FTMS packets -> CSV -> FIT" if measurements else "fixed samples -> CSV -> FIT",
              "wind_power_content": "passed" if measurements else "fixed samples verified",
              "stage": "GPS + BLE + wind + CSV/FIT" if gps else
                       "BLE + wind + CSV/FIT, no GPS" if fallback else "baseline",
              "time_source": "fallback UTC" if fallback else "NMEA UTC" if gps else "fixed UTC fixture",
              "messages": dict(counts)}
    (artifacts / "validation.json").write_text(json.dumps(report, indent=2) + "\n")


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
    if suite.startswith("system_"):
        command += ["-DHOST_POSIX_SD", "-Wl,--wrap=write", "-Wl,--wrap=fsync"]
    if suite == "concurrency" or suite.startswith("system_"):
        gps = Path(os.environ.get(
            "GPS_SOURCE_DIR", str(ROOT / ".pio/libdeps/esp32s3/TinyGPSPlus/src")
        ))
        assert (gps / "TinyGPS++.cpp").is_file(), "Set GPS_SOURCE_DIR to TinyGPSPlus/src"
        command += ["-I" + str(gps), str(gps / "TinyGPS++.cpp")]
    command += ["-o", str(binary)]
    if suite == "ui_language":
        configured = subprocess.run(
            ["cmake", "-S", str(ROOT / "test/host/ui"), "-B", str(tmp_path), "-DCMAKE_BUILD_TYPE=Debug"],
            cwd=ROOT, capture_output=True, text=True, timeout=120)
        assert configured.returncode == 0, configured.stdout + configured.stderr
        command = ["cmake", "--build", str(tmp_path), "--parallel", "2"]
    built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert built.returncode == 0, built.stdout + built.stderr
    arguments = [str(binary)]
    if suite.startswith("system_"):
        arguments.append(suite[len("system_"):])
    environment = os.environ.copy()
    if suite == "ui_language":
        ui_artifacts = ROOT / "test/results/ui-language"
        ui_artifacts.mkdir(parents=True, exist_ok=True)
        environment["UI_ARTIFACT_DIR"] = str(ui_artifacts)
    recording_suites = {"system_fit", "system_wind_power_fit",
                        "system_stage1_ble_wind_fit", "system_stage2_gps_ble_wind_fit",
                        "system_recording_lifecycle"}
    if suite in recording_suites:
        artifact_root = ROOT / "test/results/fit"
        artifact_root.mkdir(parents=True, exist_ok=True)
        artifacts = Path(tempfile.mkdtemp(prefix="recording-", dir=artifact_root))
        environment["FIT_ARTIFACT_DIR"] = str(artifacts)
    result = subprocess.run(arguments, cwd=ROOT, env=environment, capture_output=True, timeout=120)
    output = result.stdout + result.stderr
    msg_queue.put(output)
    assert result.returncode == 0, output.decode(errors="replace")
    dut.expect_exact(markers, expect_all=True, timeout=10)
    if suite == "system_recording_lifecycle":
        validate_recovered_fit(artifacts)
    elif suite in recording_suites:
        validate_recording_fit(artifacts, measurements=suite != "system_fit",
                               gps=suite == "system_stage2_gps_ble_wind_fit",
                               fallback=suite == "system_stage1_ble_wind_fit")