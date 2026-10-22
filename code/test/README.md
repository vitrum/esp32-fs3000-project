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

This builds the test image, runs four native C++ regression groups through
`pytest-embedded`, builds the ESP32-S3 Unity firmware, and executes its ten cases
using Espressif QEMU's ESP32-S3 machine and `pytest-embedded`'s `idf,qemu` services.
No board is discovered, flashed, or reset by this command.

Individual stages:

```sh
sh test/run.sh image
sh test/run.sh host
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
| Native regression | BLE scanner, manager, power/posture parsing, diagnostics, async logging, GPS, SD download, FIT export, UI guards | Mocked Arduino, NimBLE, SD and FreeRTOS |
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