# Project Testing Reference

## Entry Points

Run from the workspace root:

```sh
sh test/run.sh measurements
sh test/run.sh stages
sh test/run.sh host
sh test/run.sh build
sh test/run.sh qemu
sh test/run.sh all
```

- `measurements`: prioritized wind/power checks; report `test/results/measurements.xml`.
- `stages`: two native recording rehearsals, BLE+wind then GPS+BLE+wind; report `test/results/stages.xml`. No board flashing or browser UI emulation.
- `system_recording_lifecycle` (native pytest selection): power/wind validity gate,
  Flash commit/reboot replay, SD retry, automatic STOP/shutdown FIT and acknowledgement.
  Included in `measurements` and `all`; recovery artifacts have recovered/shutdown/batch CSV/FIT pairs.
- `host`: native pytest groups with controlled platform/hardware replacements.
- `build`: separate ESP32-S3 Unity firmware build, not production firmware.
- `qemu`: Unity firmware build and automatic menu execution on Espressif QEMU.
- `all`: native groups, Unity build and QEMU execution. Does not flash hardware.
- `image`: builds the Docker environment only.
- `hardware`: explicit Linux serial-DUT mode; replaces the board's application.

For a focused native scenario after the image is available:

```sh
docker run --rm -v "$PWD:/project" aeroprobe-idf-test:4.4.6 \
  /opt/pytest-venv/bin/python -m pytest -q test/host/test_regression.py \
  -k system_wind_power_fit --junitxml=test/results/wind-power-fit.xml
```

Use other existing scenario names with `-k` as needed. Check
`test/host/test_regression.py` and `test/run.sh` for the current inventory.

## Test Ownership

| Production behavior | Existing test home |
| --- | --- |
| Real LVGL aero layout, Chinese glyph coverage, language-switch rendering and heap/pixels | `test/host/ui_language_test.cpp`, native build under `test/host/ui/` |
| FS3000 calibration, wind vector, BME280, power key, disabled audio | `test/host/sensors_power_test.cpp` |
| BLE scanning, manager, CPS/FTMS/CSC/posture/HRS parsing and log queues (phone diagnostics removed) | `test/host/ble_diagnostics_test.cpp` |
| Dashboard layout/hit targets/gestures/rendering, SD activity and CRC layout settings | `test/host/ui_language_test.cpp`, `test/host/system_coexistence_test.cpp` |
| Explicit BLE favorites, CRC persistence, name filtering and crowded-scan priority | `test/host/ble_diagnostics_test.cpp` (`ble_runtime`) |
| Favorite-first startup/visible-scan queues, cancellation and connected-role skipping | `test/host/system_coexistence_test.cpp` (`system_storage`) |
| USB SD read-only controller, sector bounds, eject and FATFS ownership exclusion | `test/host/system_coexistence_test.cpp` (`system_usb_storage`, `system_storage`) |
| GPS receiver, download pacing/errors and navigation guard | `test/host/concurrency_test.cpp` |
| Real GPS/BLE/Wi-Fi/SD overlap, network lifecycle/routes, CSV failures and raw measurements to FIT | `test/host/system_coexistence_test.cpp` |
| FIT export failures and session handling | `test/host/fit_export_test.cpp` |
| FIT encoding, CSV validation, lengths and CRC | `test/host/fit_activity_test.cpp` |
| Production headers and FIT code on actual IDF FreeRTOS | `test/embedded/main/test_runtime.cpp` |
| Native builds and persisted FIT decoding assertions | `test/host/test_regression.py` |

Read only the test home and owning production code relevant to the change.

## Wind And Power Acceptance Rules

- Current hardware path in `src/main.cpp` uses one FS3000-1015. Do not confuse
  its m/s reading with GPS ground speed or the separate dual-probe vector code.
- FS3000 tests cover all 4096 raw codes, table interpolation, monotonicity,
  saturation, 12-bit masking, checksum/short frames and recovery. Software
  interpolation tests are not a wind-tunnel calibration certificate.
- Power tests cover CPS and FTMS, little-endian signed payloads, valid zero,
  byte boundaries, large and negative values, malformed packets and reconnect.
- Connected zero watts is valid; disconnected/missing power is not an invented
  zero-watt FIT measurement. Preserve the source and validity semantics.
- Negative signed BLE power is retained in CSV but cannot be stored as a negative
  FIT unsigned power field; current export uses the invalid value, not wraparound.
- Define invalid/freshness behavior from the actual caller before adding an
  assertion. Retaining a prior value must not be assumed to prove freshness.
- `system_wind_power_fit` feeds production FS3000 and CPS/FTMS decoders, then
  CSV and FIT, with independent raw-input expectations for every saved sample.

## Recording And FIT Validation

Use real `CsvLogger`, `FitExport` and `FitActivity` for integration. The HTTP start
test consumes `WifiWeb`'s queued command as the main loop would; it does not run
the complete main loop or a real TCP server.

The fixture currently records 65 samples into 60-row and 5-row CSV segments
across UTC midnight. Its fixed timeline is 2026-10-22 23:59:00 UTC through
2026-10-23 00:00:04 UTC: 64 seconds and 716.8 meters. These are fixture values,
not universal assertions for arbitrary recordings. If intentionally changing
the fixture, update expected inputs and summaries together and justify why.

The stage fixtures instead retain a single 65-row CSV: stage 1 uses configured
fallback UTC 2024-01-01 and no speed packets (zero distance); stage 2 parses real
NMEA text through the background receiver, uses UTC+8 date directories and GPS
speed, loses the NMEA stream for samples 20-24 and recovers at 25. Samples 22-24
have zero fallback speed; independently expected distance is 702.964 m before
FIT centimeter quantization. Both stages use raw FS3000 and CPS/FTMS inputs,
keep Wi-Fi off, and preserve input/CSV/FIT/validation artifacts. See test/README.md
for the ordered production-hardware gates; mocks do not certify the RF/UART/SD link.

The independent `fitdecode` parser must use strict CRC and parsing modes.
Validate standard power/speed/distance fields and the `airspeed`, `yaw`,
`posture`, `air_density`, `power_source` developer fields. Verify timestamps,
units, file ID, lap/session/activity summaries, timer events and record counts.
Check duplicate-save protection, malformed CSV rejection, and no completed FIT
or stale temporary file after a failed export. Reject damaged and truncated FIT.

Artifacts persist in `test/results/fit/recording-*/` through the Docker bind mount:

- `sensor-input.csv` for the raw wind/power scenario.
- `segment-0.csv`, `segment-1.csv` for actual writer output.
- `recording.fit` for the exported binary.
- `decoded.json` for independently decoded messages.
- `validation.json` for SHA256, counts and validation outcomes.

Validate the current binary's hash against its report. Do not present CRC-only
success as proof of correct measurements, or offline parsing as Garmin/Strava
upload certification. Do not commit generated artifacts as test source.

## Environment And Safety

Production uses `partitions.csv`: existing NVS/app/SPIFFS/coredump offsets remain
unchanged; an 8MB `ridecache` region starts at 4MB on the 16MB board. Update both
partition table and app only with explicit user authorization; never auto-flash,
erase the whole chip, or format SD. Recovery is a CRC-checked committed prefix,
not a guarantee against a failing card or arbitrary brownout. Actual NOR timing,
power latch, first-deployment preservation and endurance need hardware gates.

- Current Docker base: `espressif/idf:v4.4.6`, image `aeroprobe-idf-test:4.4.6`.
  Production remains Arduino-ESP32 2.0.14 / ESP-IDF 4.4.6 under PlatformIO.
- Python test dependencies live in `/opt/pytest-venv`, separate from IDF's Python.
  Versions are pinned in `test/requirements.txt`; do not upgrade them casually.
- Current pytest-embedded is 1.14.0 for the base image's Python 3.8. The 2.x
  series needs a newer Python. Independent FIT parsing uses fitdecode 0.11.0.
- System suites use `/sdcard` and GNU write/fsync fault injection inside the
  disposable Linux container. Do not run them against a real SD mount.
- With `idf.py -C`, use absolute build and SDKCONFIG paths as the runner does;
  those options have different relative-path bases.
- macOS Docker Desktop cannot directly map `/dev/cu.*` using Linux `--device`.
  Physical UART tests require host-side pytest or an explicitly configured
  passthrough setup. Follow `test/README.md`; never auto-select and flash a DUT.
- On case-insensitive host mounts, mock `Audio.h` may shadow production `audio.h`;
  use an explicit production-header path in its client test.
- Existing native shell tests are a portable subset. Docker/pytest is the
  authoritative entry point for the expanded integration suite.

## Verification Boundaries

Native mocks test software decisions, not radio scheduling or electrical timing.
QEMU runs real IDF/FreeRTOS logic but does not certify BLE discovery, Wi-Fi/BLE
coexistence, actual SD media, LCD/touch hardware or sensor accuracy. Current
Unity firmware also does not initialize the production BLE stack.

Read the hardware gates in `test/README.md` before claiming complete system
validation. `src/main.cpp` orchestration, browser/LVGL behavior and enabled MP3
playback require their own checks; the current audio configuration is disabled.