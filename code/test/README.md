# Automated Tests

The test environment uses Espressif's `espressif/idf:v4.4.6` Docker image,
matching the application's Arduino-ESP32 2.0.14 / ESP-IDF 4.4.6 toolchain.
It supports ARM64 and AMD64 Docker hosts. The application remains a PlatformIO
project; the Unity application is a separate test firmware, not a firmware migration.

## Run Without Hardware

From the workspace root:

```sh
sh test/run.sh all
```

This builds the test image, runs ten native C++ regression groups through
`pytest-embedded`, builds the ESP32-S3 Unity firmware, and executes its ten cases
using Espressif QEMU's ESP32-S3 machine and `pytest-embedded`'s `idf,qemu` services.
No board is discovered, flashed, or reset by this command.

Individual stages:

```sh
sh test/run.sh image
sh test/run.sh host
sh test/run.sh measurements
sh test/run.sh build
sh test/run.sh qemu
```

Reports and UART/process logs are written to `test/results/`. JUnit reports are
`host.xml` and `unity-qemu.xml`. The embedded report contains individual Unity
results, rather than treating a successful Python runner as proof of every case.
The first image build downloads tool dependencies and needs Internet access.
Subsequent builds use Docker layers and the pip download cache.

## Coverage And Boundaries

| Layer | Production code exercised | Hardware dependencies |
| --- | --- | --- |
| Native regression | BLE scanner/manager/decoders/diagnostics, GPS, Wi-Fi lifecycle/routes, real CSV writer, SD download, FIT export, sensor drivers, power key, disabled audio and UI guards | Mocked hardware/RTOS, POSIX files for CSV integration |
| Unity / ESP32-S3 QEMU | BLE inbox copy/FIFO, bounds/overflow, generation reset, cross-task transfer; log ring gaps/cursors; touch guards; FIT parser/writer and CRC | Real IDF FreeRTOS, emulated ESP32-S3 CPU/peripherals |
| Unity / physical ESP32-S3 | Same ten Unity cases with the board's real CPU and FreeRTOS | Physical board and serial connection |

QEMU does not validate BLE radio discovery, multi-device connection stability,
Wi-Fi/BLE coexistence, USB-CDC timing, the LCD, or physical SD-card failures.
The physical Unity application also does not initialize the production BLE stack.
Those workflows still require a separate radio/peripheral integration setup;
passing these tests must not be reported as radio coexistence certification.

Existing `test/host/run.sh` remains available for native development. The new
`test/run.sh` is the reproducible Docker/pytest entry point. C++ test assertions
remain enabled. Compilation errors, nonzero exit codes, missing PASS markers,
Unity failures, and timeouts all fail the corresponding pytest test.

## Functional Coverage

The native groups execute production `.cpp` files, not copies of their algorithms.
Each group includes multiple assertions and scenarios; ten groups do not mean
only ten individual behaviors are checked. This is a coverage inventory, not a
claim of 100% line/branch or end-to-end hardware coverage.

| Group | Scenarios |
| --- | --- |
| `system_coexistence` | GPS UART receiver and BLE scan/notifications stay active while an HTTP callback thread submits 64 control requests, 64 WebSocket telemetry frames are produced and 64 CSV samples are written; continued recording during paced HTTP download; SD lock contention, competing/missing downloads, Wi-Fi loss/reconnect, scan cancellation and clean GPS/BLE shutdown |
| `system_wifi` | No credentials, preferences failure, STA initialization/start failure, retry cooldown, low contiguous heap, mDNS failure, reconnect fallback, SmartConfig timeout/retry/save failure, enable/disable, credential clearing, invalid commands/source IDs/paths, escaped status JSON and nonfinite telemetry |
| `system_storage` | SPI/mount/task failure, no automatic formatting, GPS-over-fallback UTC priority, paused buffer overflow, date rollover, unique filenames, blocked writer recovery, short write plus failed retry, fsync failure without duplicated CSV rows, session file limits and bounded sorted directory listing |
| `system_fit` | HTTP start request, real CSV recording and writer, 65 fixed samples split at UTC midnight, asynchronous FIT export, automatic stop/flush, overwrite protection, malformed CSV rejection, retained output and independent strict FIT decoding |
| `system_wind_power_fit` | Real FS3000 frames and CPS/FTMS notification parsing feed CSV and FIT; every wind/power value is checked independently; power boundaries, quiet/full-scale wind, checksum/short packet protection, disconnect/reconnect and protocol switch |
| `sensors_power` | FS3000 calibration knots/interpolation/clamping, invalid frames/checksum/offline/reprobe; wind magnitude and yaw sign; BME280 fallback/formula/refresh/invalid samples; boot latch, short/long press and shutdown GPIOs; disabled audio rejects playback without I2S allocation |
| `concurrency` | GPS background reception, freshness/presence/checksum/UART overflow/restart; HTTP download pacing, lock timeout, short read/open/seek errors; navigation release/bounce/timer wrap |
| `ble_runtime` | Scan start/end/deadline/cancel/stop errors, adaptive scan windows, bounded packet inbox and generations, controller queue/cancellation/restart, CPS/FTMS/CSC/posture decoding, log replay/MTU/byte budget/backoff/commands/disconnect |
| `fit_export` | UTC handling, task/stop/SD failure, multiple CSV files, no overwrite, malformed/empty data and timer wrap |
| `fit_activity` | CSV validation, FIT lengths/timestamps/header/file CRC, sink failures and incomplete records |

Run selected new groups without rebuilding the image:

```sh
docker run --rm -v "$PWD:/project" aeroprobe-idf-test:4.4.6 \
  /opt/pytest-venv/bin/python -m pytest -q test/host/test_regression.py \
  -k 'system_ or sensors_power' --junitxml=test/results/system.xml
```

The system storage tests run only inside the disposable Linux container: they
use `/sdcard` as the production mount path and GNU link-time write/fsync fault
injection. They must not be run directly on a real mounted SD card. The legacy
native shell runner retains its original portable subset; the Docker entry point
is authoritative for the expanded suite.

## Recorded FIT Validation

### Wind And Power First

```sh
sh test/run.sh measurements
```

This focused entry point runs sensor calibration, BLE power boundaries and the
complete wind/power recording chain. Its report is `test/results/measurements.xml`.
FS3000 tests exercise all 4096 possible 12-bit counts, calibration intervals,
monotonicity, saturation, checksum, high-bit masking and output retention on bad
frames. BLE tests verify little-endian signed CPS/FTMS power and truncated packets.

`system_wind_power_fit` records the actual values obtained by production FS3000
and BLE decoders, rather than supplying arbitrary wind/power numbers directly to
the logger. The independent FIT validation checks these against fixed raw-input
expectations and CSV contents. A lost power connection is encoded with source
NONE and invalid FIT power, not a valid zero-watt measurement. Zero watts while
connected remains valid. Negative signed BLE power is retained in CSV but is
invalid in the FIT unsigned power field rather than wrapping to a large value.

Measurement runs retain `sensor-input.csv` alongside the CSV/FIT/JSON artifacts;
`validation.json` identifies the raw-frame measurement chain explicitly. These
checks verify software conversion and preservation, not physical wind-tunnel
calibration, sensor alignment or a power meter's real-world accuracy.

The `system_fit` test calls the production `WifiWeb`, `CsvLogger`, `FitExport`
and `FitActivity` implementations inside the disposable Linux container. The test
consumes the queued HTTP start command as the application would; it does not run
the full `main.cpp` or a real HTTP/TCP server. It records 65 samples, including
sources NONE/BLE/VIRTUAL, and switches the date after sample 60. Saving through
`FitExport::start()` stops recording, flushes the actual writer, combines both CSV
segments and publishes the FIT file only after completion.

After export, Python uses pinned independent decoder `fitdecode==0.11.0` in strict
CRC and parsing modes. It checks FIT header/length, complete record count, every
timestamp/power/cadence/speed/distance, developer field values and units, cycling
lap/session/activity summaries, and start/stop events. Header/file CRC corruption
and truncation must be rejected. NONE-source power/cadence must be invalid FIT
values, not invented zero measurements. No Garmin/Strava upload compatibility is
implied by this offline validation.

Expected recording: `2026-10-22T23:59:00+00:00` through
`2026-10-23T00:00:04+00:00`, 64 seconds, 716.8 meters.

Each run retains its own directory under `test/results/fit/recording-*/`:

- `recording.fit`: the actual exported binary, copied byte-for-byte from `/sdcard`.
- `segment-0.csv` and `segment-1.csv`: the 60-row and 5-row input segments.
- `decoded.json`: all independently decoded FIT messages and fields.
- `validation.json`: validation result, SHA256, byte count and message counts.

These artifacts are bind-mounted into the workspace and survive `docker run --rm`.
Failed validations do not create a success report. Run only this scenario with:

```sh
sh test/run.sh image
docker run --rm -v "$PWD:/project" aeroprobe-idf-test:4.4.6 \
  /opt/pytest-venv/bin/python -m pytest -q test/host/test_regression.py \
  -k system_fit --junitxml=test/results/recording-fit.xml
```

### Remaining Hardware Gates

- Run production firmware with an actual NMEA module, both BLE sensors, Wi-Fi
  WebSocket clients/downloads and a FAT32 SD card simultaneously. Repeat scans
  with diagnostics disconnected, connected and RAW enabled.
- During Wi-Fi reconnect and SD contention, compare sequence/sample counts,
  GPS UART overflow statistics, BLE queue drops and CSV/FIT integrity. Record
  observed discovery latency and disconnect rate; no simulated threshold is a
  substitute for those measurements.
- Exercise LCD/touch page transitions, real I2C/SD electrical failures, power
  latch shutdown and extended recording across day rollover.
- The actual `main.cpp` orchestration, browser UI, full LVGL rendering and enabled
  MP3/I2S/DMA playback are not executed by these host/QEMU tests. Audio is currently
  compiled with `AUDIO_ENABLE=0`; only that configuration is covered here.

## Physical Board On Linux

**This command replaces the board's application with the Unity test firmware.**
Disconnect other serial monitors first. Restore the PlatformIO application after
testing. The test console uses UART0; use a USB-to-UART adapter connected to the
board's UART0 pins if its native USB connector does not expose UART0.

```sh
ESPPORT=/dev/ttyUSB0 sh test/run.sh hardware
```

Docker maps the selected Linux device explicitly. No privileged container or
automatic selection of an arbitrary connected board is used.

## Physical Board On macOS

Docker Desktop runs Linux in a VM and cannot directly map a macOS
`/dev/cu.*` port using Linux `--device`. Build in Docker, then run the serial
pytest service on macOS. Use a Python 3.8+ environment compatible with the pinned
requirements (Python 3.11 is recommended):

```sh
sh test/run.sh build
python3.11 -m venv .venv-test
.venv-test/bin/python -m pip install -r test/requirements.txt
.venv-test/bin/python -m pytest -q -s test/embedded/pytest_runtime.py \
  --embedded-services=idf,esp --target=esp32s3 \
  --port=/dev/cu.usbserial-REPLACE --baud=115200 \
  --app-path=test/embedded --build-dir=build/esp32s3 \
  --root-logdir=test/results/hardware-logs \
  --junitxml=test/results/unity-hardware.xml
```

Replace the port with the actual UART adapter. This path also flashes the Unity
test firmware. It has not been hardware-verified unless a physical test result
is explicitly recorded.

## Version Policy

The test Python environment is separate from IDF's Python environment.
`pytest-embedded` services are pinned to 1.14.0 because the IDF 4.4.6 image uses
Python 3.8; the current 2.x framework requires Python 3.10 or newer. pytest and
esptool are pinned as well. QEMU archives are versioned and SHA256-checked.
TinyGPSPlus 1.1.0 source is pinned to a specific upstream commit so native tests
do not depend on a developer's local `.pio` cache.

References:

- https://docs.espressif.com/projects/pytest-embedded/en/latest/
- https://github.com/espressif/pytest-embedded/tree/v1.14.0
- https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32s3/api-guides/unit-tests.html
- https://github.com/espressif/qemu/releases/tag/esp-develop-9.2.2-20260417