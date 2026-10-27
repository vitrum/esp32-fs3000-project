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

This builds the test image, runs fifteen native C++ regression groups through
`pytest-embedded`, builds the ESP32-S3 Unity firmware, and executes its ten cases
using Espressif QEMU's ESP32-S3 machine and `pytest-embedded`'s `idf,qemu` services.
No board is discovered, flashed, or reset by this command.

Individual stages:

```sh
sh test/run.sh image
sh test/run.sh host
sh test/run.sh measurements
sh test/run.sh stages
sh test/run.sh build
sh test/run.sh qemu
```

Reports and UART/process logs are written to `test/results/`. JUnit reports are
`host.xml` and `unity-qemu.xml`. The embedded report contains individual Unity
results, rather than treating a successful Python runner as proof of every case.
The first image build downloads tool dependencies and needs Internet access.
Subsequent builds use Docker layers and the pip download cache.

## Coverage And Boundaries

### Type-C SD Export

The Waveshare schematic routes Type-C data to ESP32-S3 GPIO19/20, so its native
USB-OTG controller can expose the SD card as a USB Mass Storage Class device.
The production build now selects `ARDUINO_USB_MODE=0` (TinyUSB), retaining CDC
serial alongside MSC. Arduino-ESP32 stays at 2.0.14; partition offsets do not change.

Three approaches were evaluated: concurrent FATFS/host access (unsafe due to
independent filesystem caches), exclusive read/write MSC (allows accidental
deletion/formatting), and exclusive read-only MSC (selected for record export).
The firmware never shares a mounted FATFS volume with the host.

Usage after installing the firmware:

1. USB storage defaults OFF, even with a computer connected. Open SETTINGS > USB
  and select ON after stopping recording and completing FIT save/recovery.
  Active/waiting recording, FIT export, pending recovery or audio prevents
  switching. Successful SD sync and persistence trigger a restart into export.
2. The LCD shows `USB STORAGE`, `READ ONLY` and the medium/host state. Copy existing
  CSV/FIT files using Finder/Explorer. Host writes, deletes and formatting are
  rejected. If the OS offers to initialize/repair an unreadable card, cancel it.
3. Eject the disk in the computer, then select OFF on the USB settings screen.
  Firmware hides the medium, drains an active read, saves OFF and restarts into
  normal riding. Turning OFF is also available with no card/host connected.
4. The explicit setting persists under `aero-ui/usb-msc`; reconnecting a cable
  does not enable it. Ejection hides the medium but leaves USB mode selected
  until OFF is pressed. A failed read returns a SCSI error without permanently
  withdrawing the medium. Do not hold BOOT before reset/power-on unless
  intentionally entering the ESP32 ROM downloader.

Normal riding never switches SD ownership on cable insertion. It exposes an
empty MSC drive (no medium) while CDC remains usable. Mode changes intentionally
use a safe restart rather than hot-unmounting the live FATFS/writer. There is no
automatic host detection window or BOOT override for mode selection anymore.

In export mode, SD is initialized only as a raw SDSPI block device: FATFS, logger
writer, Flash-journal recovery, recording, BLE, Wi-Fi and audio never start.
Unreplayed Flash records remain pending until a normal riding boot. Raw reads
use a bounded internal DMA sector buffer, handle byte offsets/cross-sector
requests and reject out-of-range access. There is no SD write callback path.
SCSI write protection is supplied by a strong `tud_msc_is_writable_cb` override
in the entry point (outside TinyUSB's weak callback declarations), and WRITE10
also returns DATA PROTECT. No automatic card formatting is used.

MSC revision `1.1` logs `firmware=ro-msc-1.1` and its build time. Read errors
distinguish NOT READY/medium absent (02/3a), invalid field (05/24), out-of-range
LBA (05/21), and read failure (03/11). Invalid host requests and transient read
failures do not latch the disk offline; a subsequent valid read can succeed.

Mac validation on 2026-10-27: this build was flashed and independently verified;
its startup identifier matched, and `diskutil info` reported `SD READ ONLY` and
`Media Read-Only: Yes`. `diskutil verifyVolume` ran `fsck_msdos -n` through all
three FAT phases, returning 0 with only an unset FSInfo free-count warning.
No MSC read error was captured during that check. Nevertheless,
`diskutil mount readOnly` returned `SUIS premount dissented`. This is not a
successful mount, nor proof that host writes are required. The local `diskutil`
manual explicitly supports read-only mounting. Next diagnosis is a direct
read-only FAT mount with user-controlled administrator access; do not silently
switch to writable export, repair the FAT or reformat the card.

TinyUSB CDC can acquire a different serial device name. First deployment may
require BOOT+RESET to enter the ROM downloader, then selecting the newly
enumerated serial port; don't erase Flash/NVS or update the partition table for
this change. USB-Serial/JTAG debugging is not simultaneous with USB-OTG on the
same internal PHY. The USB full-speed and SDSPI link are not USB 3.x storage.

Upload recovery: `No serial data received` after a successful build means the
ROM handshake failed, not that Flash was written or the build is corrupt.
On the pinned PlatformIO platform the actual reset keys are
`board_upload.before_reset`/`after_reset`, not `upload_resetmethod`. The default
upload now performs a 1200bps CDC touch, waits for the new ROM port and uses
`no_reset` before transfer. `Serial.enableReboot(true)` preserves that mechanism.
A disappearing application port during the touch is expected; the ROM port has
a different name. Verify the same chip MAC before writing.

If CDC is unresponsive, hold BOOT, press/release RESET, then release BOOT and
re-enumerate serial ports. Use the ESP32-S3 ROM with `--before no_reset --no-stub`
at 115200. After comparing the on-device partition table with the build, an
application-only `write_flash 0x10000 .pio/build/esp32s3/firmware.bin` preserves
bootloader, partition table, NVS and ridecache; verify Flash afterwards. Never
use chip erase, guessed offsets, another USB device's port or `--force` to bypass
security checks. A ROM port already entered manually does not need another
1200bps touch.

`system_usb_storage` exercises the production export controller and real
CsvLogger ownership guard: boot-only selection, no-medium/failed media, rejected
writes, partial/cross-sector reads, last-byte bounds, 32-bit-overflow inputs,
ejection, persisted ON/OFF/default/corrupt/write-failure settings, and refusal
to mount/start recording while USB owns SD. Recording lifecycle tests also
verify that pending recording/saving blocks USB restart.
`system_storage` reruns the block-controller cases with ordinary storage tests.
Production builds verify TinyUSB/SDSPI integration, but mocks/QEMU cannot prove
USB enumeration, Mac/Windows mounting, throughput, physical SD removal or cable
and power behavior. Hardware acceptance must verify read-only mounting, copy
and compare file hashes, eject/unplug/replug, missing card, manual ON/OFF and
unchanged recording after a normal boot. No board is auto-flashed.

Sources: [Waveshare board schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8.pdf),
[Espressif USB MSC API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/usb_msc.html),
and the installed Arduino-ESP32 2.0.14 USBMSC/TinyUSB implementation (the pinned
version's actual callbacks take precedence over newer API examples).

### BLE Favorites And Discovery

The POWER SOURCE panel exposes a star toggle for the current, successfully
connected CPS/FTMS device. CSC-only and disconnected devices cannot be added.
Up to four explicit favorites persist in the separate CRC-checked
`aero-links/favorites` NVS key; existing `profiles` Wi-Fi/BLE history is unchanged.
Full storage or a write failure preserves previous favorites and reports failure.
Opening POWER SOURCE again allows the connected device to be unstarred.

Startup tries favorites before connection history, without duplicate attempts.
Manual discovery returns up to 16 devices in a vertically scrollable region
with fixed header/actions. Favorites sort before supported sensor services,
then RSSI. Each visible favorite gets one automatic connection attempt if no
power device is connected; cancellation stops the queue. Already-connected
power devices are never replaced merely because a scan completed.

Three filtering approaches were evaluated: name-only blocking (simple but
false positives), service-only admission (misses incomplete advertisements),
and explicit favorites plus name filtering and service priority (selected).
Non-favorite connectable advertisements containing ASCII `bike`, ignoring case,
are hidden before result truncation. Explicit favorites override this rule.
Named unknown devices remain manually selectable for compatibility.

Sources: [Bluetooth SIG Assigned Numbers](https://www.bluetooth.com/specifications/assigned-numbers/)
and [NimBLE 2.5.1 advertised-device API](https://h2zero.github.io/NimBLE-Arduino/class_nim_b_l_e_advertised_device.html).
Service UUIDs identify profiles, not shared-bike ownership; local names are
self-declared and may be absent. This is a noise filter, not a complete or
authenticated shared-bike detector. No speculative manufacturer/MAC blacklist
is applied. Unbonded devices that rotate private addresses may need rediscovery.

`ble_runtime` tests production NVS/scan behavior for reboot, CRC corruption,
write failure, capacity, typed addresses, mixed-case names and a favorite after
48 other advertisements. `system_storage` covers startup priority, serialized
scan favorites, already-connected skipping and cancellation. Browser regression
adds star persistence, automatic reconnect, unstar, filtering and list scrolling.
RF discovery, physical touch scrolling and actual NVS power cycling still need
a board; no automatic flashing is performed.

### Dashboard And Live Web UI

The 240x320 dashboard is a production `DashboardView` with a 28px black status
bar, white 16px GPS/settings/clock content, and SD activity colors. Horizontal
padding is 2px at the outer status edges. SD and settings each have a separate
44x44px transparent hit target, at x=48 and x=94. These do not overlap one another.
Data starts at y=44, outside both hit targets; the visual bar remains only 28px.

Five unframed rows (Zone/CdA, wind, power, cadence, heart rate) have fixed left
labels/units and right-aligned values. A stationary press of at least 80ms followed
by release cycles 20/28/40/48px preferred
number sizes, using 36/44/56/64px row heights. A 272px budget with 2px separators
fits three largest rows and two compact rows. Most recently enlarged rows have
priority; other effective levels shrink as needed, never hiding a data group.
Numbers that exceed their column use a smaller font without resizing the row.
Large aero rows show a short posture description; compact rows retain CdA.
Touches shorter than 80ms do not resize; stationary holds have no upper cutoff
and resize only once on release. The previous narrow 300..699ms window was
removed because natural short taps and deliberate long presses were both lost.
Accepted resize/reorder releases must be at least 180ms apart. A new press may
begin inside that interval and still succeed if its release is late enough.
Motion under 12px is tolerated as finger jitter. Hold at least 600ms and move
vertically at least 12px to reorder; horizontal swipes do not become drags.
Pointer capture/press lock
keeps the original row, a target is highlighted, and release commits one action.
Horizontal swipes, canceled/outside drops and drag releases cannot become taps.
Long-press the refresh icon in SETTINGS to restore the default layout.

Layout order/effective levels persist after 800ms idle in CRC/version-checked
`aero-ui/dashboard`; invalid stores fall back to defaults, failed writes retry
without replacing the last saved blob. Language changes keep readings/order.
SD grey means unmounted/unavailable, green means available/idle, red plus a
250ms toggled red dot means actual SD write activity (including CSV/FIT metadata).
A 600ms visual hold exposes brief writes. Write failure shows a steady red
warning, not a forever-blinking activity dot. Tracking uses actual SDSPI write
commands; RAM buffering, Flash journal/NVS and SD/USB reads do not count as writes.
Physical card removal becomes known when a subsequent status/I/O check fails.

Phone BLE diagnostics, replay GATT, advertising and command reception have been
removed. USB serial logging and bounded sensor inboxes remain. Central roles are
power, posture and heart rate; the existing three-link budget is retained.
Standard HRS 0x180D/0x2A37 heart notifications support uint8/uint16 bpm, contact
flags, energy and RR bounds. Disconnect, unsupported contact and >5s stale data
display `--`; they are not valid zero samples. Successful heart connections are
saved with their address type/role and tried once at startup. Discover them from
SETTINGS > POWER > SCAN BLE DEVICES; HRS rows connect to the heart client.
This iteration adds heart rate to LCD and Web only (`hr` integer or null);
CSV, FIT and Flash journal schemas remain unchanged. Heart-rate recording/export
requires a separate versioned schema/recovery change; it is not claimed here.

Native tests enumerate all 1024 five-row size combinations, render with actual
LVGL in both languages, verify hitbox separation/text/heap bounds, and exercise
gesture timing/cancel/wrap, NVS corruption/write failure, SD writes and HRS packets.
Browser regression runs 126 checks at desktop/mobile sizes; real pointer tests
cover tap, hold-drag and clicks in the 28..44px expanded header area.
The live Web page keeps CdA as its primary
aero value with the same unframed visual structure: one column on
phones, instrument plus supporting controls/records on desktop. It has no
external font/icon/CDN dependency. Web airspeed and ground speed display km/h;
production telemetry, CSV and FIT retain their existing m/s units.

Web telemetry adds the LCD's current estimated `cda` and UTC `utc`; the main loop
refreshes the LCD estimate before broadcasting. Missing/invalid wind suppresses
CdA. This does not change the existing approximate CdA model or constitute
calibration. Realtime values are cleared on disconnect, bad JSON or a five-second
data timeout, while command errors remain independent from incoming telemetry.

With the workspace served over local HTTP:

- `simulator/index.html?test=1`: device UI regression, including alignment,
  separate units, unframed rows, top settings and the shared GPS status slot.
- `simulator/web-preview.html?test=1`: loads the actual HTML from
  `src/web_assets.h`, installs simulated WebSocket/HTTP boundaries and runs Web
  regression. Without `?test=1` it is an interactive preview, not a device client.

Web checks include null versus valid zero, km/h conversion, large signed power,
invalid wind/CdA, start/stop requests, settings, record-list failures, safe file
names, successful/failed downloads, duplicate sockets, malformed frames and
disconnect recovery. Validate 1440px desktop, 390px mobile and 320px narrow mobile
with Playwright screenshots, bounding boxes, overflow checks and canvas pixels.
Actual LCD rendering/touch targets and the real network remain hardware gates.

| Layer | Production code exercised | Hardware dependencies |
| --- | --- | --- |
| Native regression | BLE scanner/manager/decoders/HRS, GPS, Wi-Fi lifecycle/routes, real CSV writer, SD download, FIT export, sensor drivers, power key, disabled audio and UI guards | Mocked hardware/RTOS, POSIX files for CSV integration |
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
Each group includes multiple assertions and scenarios; fifteen groups do not mean
only fifteen individual behaviors are checked. This is a coverage inventory, not a
claim of 100% line/branch or end-to-end hardware coverage.

| Group | Scenarios |
| --- | --- |
| `ui_language` | Actual LVGL C rendering, production AeroZoneView and generated Chinese font; all UI CJK glyphs/icons, text metrics, all zones in both languages, 1600 refreshes, heap integrity and nonblank pixels |
| `system_coexistence` | GPS UART receiver and BLE scan/notifications stay active while an HTTP callback thread submits 64 control requests, 64 WebSocket telemetry frames are produced and 64 CSV samples are written; continued recording during paced HTTP download; SD lock contention, competing/missing downloads, Wi-Fi loss/reconnect, scan cancellation and clean GPS/BLE shutdown |
| `system_wifi` | No credentials, preferences failure, STA initialization/start failure, low contiguous heap, mDNS failure, saved-network ordering, immediate failure/timeout progression, stop-after-list, manual reconnect, successful-connection persistence, SmartConfig timeout/manual retry/save failure, enable/disable, credential clearing, invalid controls/paths, escaped JSON and nonfinite telemetry |
| `system_storage` | SPI/mount/task failure and startup worker retry/reuse, bounded CMD13 timeout recovery (no write-command or non-timeout retries), no automatic formatting, recording-directory file conflicts, GPS-over-fallback UTC priority, paused buffer overflow, date rollover, unique filenames, blocked writer recovery, short write/fsync recovery, session limits, repeated parent/child directory reads, empty versus missing directories, partial-list errors and recovery |
| `system_usb_storage` | Read-only USB export controller, boot-only ownership, partial/cross-sector reads, bounds/overflow, failures/eject and real CsvLogger mount/start exclusion |
| `system_recording_lifecycle` | Production RideRecorder and raw Flash journal: validity-gated start, valid zero samples, duplicate START, skipped windows/duration, torn session/sample/ack writes, CRC corruption, full cache, SD recovery retry, four-sample and 65-sample reboot replay, shutdown FIT and delayed cache acknowledgement; three retained CSV/FIT pairs independently decoded |
| `system_fit` | HTTP start request, real CSV recording and writer, 65 fixed samples split at UTC midnight, asynchronous FIT export, automatic stop/flush, overwrite protection, malformed CSV rejection, retained output and independent strict FIT decoding |
| `system_wind_power_fit` | Real FS3000 frames and CPS/FTMS notification parsing feed CSV and FIT; every wind/power value is checked independently; power boundaries, quiet/full-scale wind, checksum/short packet protection, disconnect/reconnect and protocol switch |
| `system_stage1_ble_wind_fit` | Wi-Fi off, GPS absent, raw FS3000 + CPS/FTMS packets, BLE disconnect/reconnect, one 65-row CSV, fallback UTC and independently decoded FIT |
| `system_stage2_gps_ble_wind_fit` | Stage 1 measurement inputs plus background TinyGPSPlus NMEA reception, GPS UTC/local date, GPS speed override, checksum error, 5-second stream loss/recovery and uninterrupted 65-row CSV/FIT |
| `sensors_power` | FS3000 calibration knots/interpolation/clamping, invalid frames/checksum/offline/reprobe; wind magnitude and yaw sign; BME280 fallback/formula/refresh/invalid samples; boot latch, short/long press and shutdown GPIOs; disabled audio rejects playback without I2S allocation |
| `concurrency` | GPS background reception, valid-NMEA presence, offline timeout, noise rejection, automatic reconnect after disconnect or initial absence, freshness/checksum/UART overflow/restart; HTTP download pacing, lock timeout, short read/open/seek errors; navigation release/bounce/timer wrap |
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

Production uses the Waveshare board's 8MB OPI PSRAM for the 128KB LVGL pool
(internal RAM fallback), leaving internal RAM available for BLE, task stacks
and SD DMA. Compile with `pio run -e esp32s3`; mocks/QEMU do not validate PSRAM
electrical operation or LCD animations. On hardware, check startup `[MEM]`
PSRAM size, `[SD] recording worker ready`, directory browsing, CSV/FIT recording
and GPS disconnect/reconnect. CMD13 timeout retries remain bounded and do not
hide persistent card failures; never automatically format a failing card.

The browser reports `SD read error` rather than treating failed `stat/readdir`
operations as an empty card; successful entries in a partial list remain visible.
Capacity truncation and the 400-entry scan limit report `EOVERFLOW`, rendered as
`Partial list`, rather than success or a card read error. The exact scan-limit
boundary is tested; actual lookup/close errors take precedence over truncation.
Browser status is retained across language refreshes and cleared by a successful
reload. Native tests cover invalid arguments, absent media and SD-lock contention;
the LVGL status rendering still requires hardware verification.
Recording directories use POSIX lookup/create/recheck, so a regular file named
`LOG` or a directory that cannot be read back fails before CSV creation.
Changing directory names across repeated physical reads still requires backing
up and checking the card on a computer, then comparing against board-side reads.
Native POSIX tests do not establish that the physical FAT or SPI link is healthy.

## Recorded FIT Validation

### Chinese And English UI

Language selection is restored and persisted as `aero-ui/zh`; boot reads the
preference instead of forcibly clearing it. Numeric fields/units/icons keep
Montserrat. Localized text uses a 14px, 4bpp, uncompressed Noto subset with the
existing Montserrat 14 fallback, avoiding runtime glyph decompression. The
subset covers the current UI string literals, not arbitrary Chinese device/file
names. Unrecognized external characters still require a larger future font.

Problems addressed: old English waiting/status captions exceeded their 84px
column, independent update paths duplicated text/formatting, the CJK subset
omitted newly added labels, and language refresh still applied the obsolete
single record/stop-toggle text. The new aero component owns fixed text bounds
and both value/language updates. Language refresh preserves the separate
START/STOP buttons and the current waiting/saving state; log/time/setup labels
are localized too. CSV/FIT values, intervals and recording state are unchanged.

Rebuild the font after adding translated UI literals:

```sh
curl -fL -o /tmp/aeroprobe-NotoSansCJKsc-Regular.otf \
  https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf
node tools/build_ui_font.mjs /tmp/aeroprobe-NotoSansCJKsc-Regular.otf
```

The script pins `lv_font_conv@1.5.3` and checks source SHA256
`2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`.
The font's existing OFL license remains in `src/NotoSansCJK-OFL.txt`.
Do not disable glyph tests or use a full CJK font merely to hide missing labels.

`ui_language` compiles the real LVGL C sources with one consistent native config,
the production component in C++11 and actual generated font. It reproduces the
old over-width caption, checks every current CJK literal and fallback icon,
measures rectangles and text in both languages for zones 0-7, renders 1600
frames, checks LVGL heap integrity and blue Zone pixels, and saves real renders
to `test/results/ui-language/aero-en.ppm` and `aero-zh.ppm`. This is stronger than
browser CSS-only checks, but still does not emulate the ST7789 bus or the whole
screen/menu orchestration. Physical stress tests of language switches, all
pages, NVS reboot preference and concurrent BLE/GPS/SD remain hardware gates.

Focused check:

```sh
docker run --rm -v "$PWD:/project" aeroprobe-idf-test:4.4.6 \
  /opt/pytest-venv/bin/python -m pytest -q test/host/test_regression.py \
  -k ui_language --junitxml=test/results/ui-language.xml
```

### Saved Connections And Startup Queue

Successful Wi-Fi and BLE power/posture connections are stored in NVS namespace
`aero-links` as a versioned CRC-checked blob. Each list holds at most four entries,
most-recent-success first; SSID and BLE address/type/role are deduplicated. BLE
address type is preserved. Failed connections and failed writes do not replace
the in-memory or saved list. The first load may create the NVS namespace. Saved
credentials use the board's existing NVS security configuration; this is not a
new encryption/bonding layer, and passwords are never printed in logs.

On boot, legacy `aero-wifi` credentials remain readable for migration. The
firmware tries the saved Wi-Fi list once, at most 15 seconds per pending station
attempt; immediate failures advance to the next entry. A successful station
connection is persisted and starts NTP. The BLE startup list begins only when
Wi-Fi is no longer connecting/provisioning and the BLE worker is idle. It uses
saved addresses directly, one queued command at a time. Once power or posture
has connected, further entries for that role are skipped. Failures advance once;
there is no periodic background power/posture scan or automatic Wi-Fi reconnect.
If a connected network drops, Wi-Fi stops and waits for explicit manual action.

Manual controls take over the remaining startup list. ON retries saved Wi-Fi;
SET UP WI-FI explicitly starts SmartConfig, including when already OFF. A
SmartConfig receipt is not enough to save new credentials: actual station
connection must succeed. Failed/exhausted attempts do not automatically enter
SmartConfig, and SmartConfig timeout does not restart itself. BLE scanning still
runs on explicit request; devices advertising 0xFFE0 or named WT* can be selected
as posture sensors. Successful manual connections update the saved list. Public
and static-random addresses can be reused, but an unbonded peripheral rotating
its private address may require manual rediscovery; saving an address is not
Bluetooth bonding. Each boot necessarily establishes new physical radio links.

`system_storage` tests list bounds/order, reboot load, corruption and write failure,
startup serialization/role skipping/exhaustion/manual cancellation. `ble_runtime`
checks successful persistence and posture direct connection without scanning,
including after discovery backoff. `system_wifi` checks multi-network attempts,
success-only saves and no unattended retries; `system_coexistence` exercises an
explicit manual Wi-Fi reconnect while other production modules keep running.
Real AP availability, RF coexistence and pairing of the actual devices remain
hardware gates. Main-loop orchestration is compiled but not run by native mocks.

### Recording And Power-Loss Recovery

START and STOP are separate fixed-position buttons. START arms the recorder;
the first 1Hz window containing fresh power and valid wind creates the recording.
The production BLE power validity window is 3 seconds; connection alone and CSC
cadence notifications are not power. Valid 0W and 0m/s wind are accepted. Explicit
VIRTUAL input is accepted for testing. Invalid windows are skipped, not stored as
invented zero measurements; the session timer continues and shows skipped count.
STOP cancels waiting or finalizes CSV/FIT automatically. Saving prevents a new
START. Long-press shutdown waits for FIT publication and Flash acknowledgement;
failure retains data and power, with local SAVE FIT available for recovery retry.

| Storage option | Suitable role | Power-loss limitation |
| --- | --- | --- |
| RAM/PSRAM | Fast existing sample batches | Volatile, cannot recover after power loss |
| Shorter SD flush interval | Smaller normal buffering window | Still depends on SD and in-flight FAT writes |
| NVS | Small settings/metadata | Existing 20KB partition is unsuitable for continuous ride samples |
| Dedicated append-only Flash journal (selected) | Durable committed samples plus existing SD CSV/FIT | Finite capacity/endurance; in-flight sample can be lost; physical brownout testing still required |

The selected layout in `partitions.csv` keeps NVS, application, SPIFFS and
coredump at their previous offsets, adds `ridecache` at 0x400000 with size
0x800000, and requires the board's 16MB Flash. It does not move existing data
partitions or format the SD. **Back up device and SD data before deployment.**
The partition table and application must both be updated using the configured
PlatformIO upload, not an app-only flash. Do not erase the whole chip. An old
partition table or unavailable/corrupt cache disables new production recordings
instead of silently claiming power-loss protection. No board was auto-flashed.

Each journal record is 64 bytes: versioned magic, session/index/type, payload,
CRC32 and a separately programmed commit word. Payload is written before commit.
Time-source/UTC-to-millis reference is included, so recovery does not substitute
the reboot's clock for the previous ride. A sequential 4KB sector is erased only
when first used by a new session, not for every sample. Header plus sample records
provide about 36 hours at 1Hz, slightly less with time-reference upgrades. Full
cache stops accepting samples and finalizes the committed prefix; it never wraps
over unsaved data. This is not wear-leveled archival storage: erase endurance,
especially the first metadata sector per ride, depends on the actual Flash chip.

At boot, an unfinished session is replayed by a background task into new CSV/FIT
files under `/LOG/R<session-id>/`. A short final uncommitted record is ignored;
CRC damage in a committed record blocks automatic recovery/erasure. Replay flushes
every 60 samples to avoid RAM backlog loss. If normal CSV buffering dropped rows,
the same journal replay is used before FIT completion. New recording is blocked
until the pending session is recovered. Flash is acknowledged only after FIT is
published. SD errors or acknowledgement failure preserve the cache for retry.
Power loss after publishing FIT but before acknowledgement can create another
recovered copy on reboot; original files are never overwritten. Successful normal
CSV files are retained, including pre-recovery copies. This journal supports one
pending session, not multiple offline rides or recording without an initially
writable SD card.

Limits: the current in-flight sample/Flash operation can be lost, and actual
power failure can also damage SD media. This is committed-prefix recovery, not
zero-loss certification. Use the known-good card; validate Flash erase/write
latency with BLE/GPS active and exercise the real power latch without USB keeping
the board powered. Do not treat mock/QEMU tests as brownout or endurance proof.

Focused check:

```sh
docker run --rm -v "$PWD:/project" aeroprobe-idf-test:4.4.6 \
  /opt/pytest-venv/bin/python -m pytest -q test/host/test_regression.py \
  -k 'system_storage or system_recording_lifecycle' --junitxml=test/results/recording-recovery.xml
```

The lifecycle scenario retains `recovered.csv/fit` (4 valid rows, 5 seconds,
41.5m), `shutdown.csv/fit` (2 rows, 1 second, 9m), and `batch.csv/fit` (65 rows,
64 seconds, 512m) plus `validation.json` under a new `test/results/fit/recording-*`.
It verifies actual production journal/controller/writer/exporter behavior using
NOR 1-to-0 and torn-write mocks, then independently decodes timestamps, all power,
wind, cadence/speed, developer fields/units, distances, events and summaries.
The lower-level logger fixtures intentionally still test legacy/missing-source
rows; production RideRecorder filters invalid windows before reaching the logger.

### Wi-Fi Network Time

After a station connection the firmware starts ESP-IDF SNTP asynchronously with
`ntp.aliyun.com`, `time.cloudflare.com` and `pool.ntp.org`. No blocking
`getLocalTime()` wait is used. Until an actual SNTP completion the network clock
is untrusted; DNS/server failure leaves the dashboard at `--:--:--` unless GPS
provides time. SNTP manages retries and periodic resynchronization. A previously
synchronized system clock continues through disconnection or Wi-Fi OFF, but is
not retained across power loss. Reconnection requests synchronization again.

The dashboard uses valid GPS UTC first, then synchronized network UTC, and shows
UTC+8 `HH:MM:SS` at the top right. The small recording icon is gray when stopped
and green while recording; tapping opens RIDE LOG and BACK returns to its origin.
NTP can replace a fallback recording UTC anchor; GPS remains higher priority.
Repeated NTP updates do not move an already selected network anchor. CSV date
directories use UTC+8, FIT timestamps remain UTC, and sample timing stays based
on `millis()` rather than a corrected wall clock.

`system_wifi` covers pending/success/reconnect and retained time while offline;
`system_storage` covers NTP/fallback/GPS priority and anchor retention;
`system_fit` independently decodes the real FIT generated from a network-reference
fixture. The browser regression models server unavailability, completion and
holdover, but does not contact real NTP servers. Physical verification: connect
Wi-Fi with Internet/UDP 123 access, wait for `[TIME] NTP synchronized`, compare the
clock with local UTC+8 time, disable Wi-Fi and confirm it still advances, then
record/export without GPS and check the resulting FIT UTC date. DNS/NTP service
availability and actual LCD rendering are hardware gates, not mock guarantees.

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

### Two-Stage Recording Rehearsal

```sh
sh test/run.sh stages
```

This is a native production-module simulation, not browser UI or RF emulation.
It does not flash/reset the board. Report: `test/results/stages.xml`.
`measurements` also includes these two scenarios and recording lifecycle (six groups total).

Both scenarios feed the real FS3000 driver and BLE CPS/FTMS decoder, writer and
FIT exporter. They test valid zero watts, negative signed power, BLE loss at
samples 30-31, reconnect/protocol switch at 32, wind checksum rejection and
truncated BLE packets. All 65 saved records are independently checked, not just
the export return value. Wi-Fi stays off to match the initial hardware stages.

| Stage | Time and speed | Expected artifacts |
| --- | --- | --- |
| 1: BLE + wind + logging | No GPS; configured fallback UTC 2024-01-01; these packets contain no wheel speed, so ground speed/distance are zero | One 65-row CSV under `/LOG`, FIT with 65 records, 64 seconds, zero distance |
| 2: GPS + BLE + wind + logging | NMEA UTC starts 2026-10-22 23:59:00; UTC+8 directory is 2026-10-23; 22.4 knots becomes 11.524 m/s; no NMEA at samples 20-24, offline/zero fallback speed at 22-24, recovery at 25 | One 65-row CSV and FIT, 64 seconds, 702.964 m before FIT centimeter quantization |

Find each run in `test/results/fit/recording-*/validation.json` using its `stage`
and `time_source`. The folder contains `sensor-input.csv`, `segment-0.csv`,
`recording.fit`, `decoded.json` and `validation.json`; stage 2 additionally saves
`gps-input.nmea` (60 valid sentences plus one bad-checksum sentence).
The stages call production modules as a harness; they do not execute `main.cpp`
or model real I2C/UART/RF timing. The existing browser simulator remains a
separate UI/demo tool, not proof of firmware measurement accuracy.

### Production Hardware Sequence

Use the known-good card and existing production firmware, not `run.sh hardware`
(that command replaces it with Unity firmware). Capture the serial monitor from
boot. Leave Wi-Fi off and audio disabled in both initial stages.

1. Stage 1: disconnect GPS, select the real BLE power meter (not VIRTUAL), verify
  changing watts and source BLE, then record for at least 120 seconds. Include
  coasting/zero watts and controlled airflow over the FS3000. Stop, save FIT,
  retain CSV/FIT and serial logs. Require no SD errors or dropped samples;
  compare steady power and wind values across monitor, CSV and decoded FIT.
  Zero watts with BLE connected must stay valid; missing BLE must not become
  valid zero power. No wheel-speed packets means zero speed/distance is expected.
  Fallback UTC is not a real ride date. Back up the card before stage 2.
2. Stage 2: power down before changing GPS wiring, then restart outside and wait
  for fresh FIX/UTC. Repeat the same recording, confirm the UTC+8 date directory
  and FIT UTC timestamps, and compare GPS ground speed separately from airspeed.
  Exercise loss/recovery by safely interrupting only the NMEA stream (never
  rewire power while running). Logging must continue, GPS must become offline,
  ground speed must fall back to BLE wheel speed or zero, then recover.
3. Accept each stage only after independently decoding its real FIT and comparing
  count, power, airspeed, source IDs, UTC, duration and distance against retained
  CSV/monitor data. Record device identities, firmware revision and test times.
  These are manual gates; the automated simulation does not mark them passed.

### Remaining Hardware Gates

- Run production firmware with an actual NMEA module, power/posture/heart BLE
  sensors, Wi-Fi WebSocket clients/downloads and a FAT32 SD card simultaneously.
  Repeat scans while checking power packet continuity and heart freshness.
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