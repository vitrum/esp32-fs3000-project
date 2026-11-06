---
name: esp32-buttonless-flash
description: 'Use when flashing or uploading this Waveshare ESP32-S3-Touch-LCD-2.8 firmware on macOS without pressing BOOT or RESET, using 1200bps TinyUSB CDC touch, finding the re-enumerated ROM port, app-only esptool write/verify, or monitoring startup logs. Covers broken RESET buttons and the limits of automatic application restart.'
---

# ESP32 Buttonless Application Flash

## Scope And Safety

Run from the workspace root. This is the verified recovery workflow for this
project's Arduino-ESP32 2.0.14 / ESP-IDF 4.4.6 firmware and esptool 4.5.1.
It does not require BOOT/RESET to enter download mode and write the application.
It does NOT guarantee automatic application restart: this board has previously
remained in ROM until the user fully power-cycled it.

- Require explicit authorization to flash a physical board. A request to build,
  inspect, test, or load this skill alone is not permission to flash.
- Stop recording and finish CSV/FIT/recovery writes before triggering download.
  If recording state is unknown, have the user confirm it is safe to restart.
- Preserve existing workspace edits. Do not commit, change branches, upgrade
  packages, or rewrite upload configuration just to make recovery work.
- Default to application-only at `0x10000`. Do not erase the chip, format SD,
  write NVS/ridecache, use `--force`, or change partition/bootloader images.
- If a feature needs a partition-table migration, stop and obtain separate
  authorization. App-only flashing cannot deploy a new partition layout.
- Do not use `tools/flash_chunked.sh` for this procedure: its current version
  also writes bootloader and partition-table images.
- Keep ports, image path, chip identity, write verification and boot validation
  distinct. A digest match is not proof the application booted or sensors work.

## 1. Identify The Board And Build

The host `pio` command may not be on PATH. Use the existing project environment:

```sh
"$HOME/.platformio/penv/bin/pio" run -e esp32s3
"$HOME/.platformio/penv/bin/python" -m serial.tools.list_ports -v
```

Require a successful current build before using
`.pio/build/esp32s3/firmware.bin`. Retain known regression failures in the report;
do not describe them as passing because a production build succeeds.

Identify by USB serial number/MAC and physical USB location, not the board
description or port suffix alone. Previously verified hardware:

| State | Example port | USB serial |
| --- | --- | --- |
| Application TinyUSB CDC | `/dev/cu.usbmodemD83BDA9B6F682` | `D83BDA9B6F68` |
| ESP32-S3 ROM USB JTAG/serial | `/dev/cu.usbmodem1101` | `D8:3B:DA:9B:6F:68` |

Both can use VID:PID `303A:1001`. Normalize serial punctuation/case when comparing.
Actual port names may change. The USB descriptor's default 8 MB/no-PSRAM board
name is not proof of this board's physical memory configuration.

Stop this agent's serial monitor before opening the same port for flashing.
If another process owns the port, identify it; do not kill unrelated processes.

## 2. Enter ROM Without Buttons

If the confirmed board already exposes the hardware USB JTAG/serial interface,
skip the CDC trigger, but still confirm a ROM handshake. The descriptor alone
does not prove ROM is responsive.
If the application CDC is present, substitute its current port into this command:

```sh
"$HOME/.platformio/penv/bin/python" -c 'import serial; port = serial.Serial(port=None, baudrate=1200, timeout=1); port.dtr = True; port.rts = True; port.port = "/dev/cu.usbmodemD83BDA9B6F682"; port.open(); print("1200bps CDC download trigger sent"); port.close()'
```

This changes USB line coding to 1200bps; the running TinyUSB application requests
ROM download mode. The port can disappear while closing, causing a device-not-
configured error. That error alone does not establish failure or success.

Re-enumerate after the trigger:

```sh
"$HOME/.platformio/penv/bin/python" -m serial.tools.list_ports -v
```

Require the matching board's ROM port before writing. Do not reuse the application
port after it disappears. Do not run sleep loops or indefinitely poll ports; use
bounded tool execution and react to re-enumeration or ask for reconnection if
the required interface never appears.

`default_reset` and `usb_reset` previously failed to handshake with this running
application. The explicit CDC trigger plus ROM `--before no_reset` succeeded.
Do not repeatedly retry the failed combinations or conclude the board is damaged.
Do not silently enable PlatformIO's automatic 1200bps port tracking: that is a
different upload path, with previously observed stale-port selection failures.

For a confirmed hardware USB JTAG/serial port that fails `--before no_reset`,
try one non-writing identification with hardware USB reset:

```sh
"$HOME/.platformio/penv/bin/python" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 115200 --no-stub --before usb_reset --after no_reset chip_id
```

This succeeded on 2026-11-06 with MAC `d8:3b:da:9b:6f:68`. Subsequent processes
still failed with `no_reset`; using `usb_reset` before EACH connection worked.
That result is specific to the hardware USB interface, not an instruction to
apply USB reset to the application TinyUSB CDC port. Do not retry an identical
failed handshake indefinitely.

## 3. Write Only The Application And Verify

### Preferred: Bounded App-Only Chunks

Use the bundled [application writer](./scripts/flash_app.py). It defaults to a
dry run that opens no serial port, parses the current partition CSV, checks the
known app boundary, and lists 96 KB chunks. The explicit `--flash` switch is
only permitted after the user authorized a physical write.

```sh
"$HOME/.platformio/penv/bin/python" -m unittest discover -s .github/skills/esp32-buttonless-flash/scripts -p test_flash_app.py -v
"$HOME/.platformio/penv/bin/python" .github/skills/esp32-buttonless-flash/scripts/flash_app.py --serial D8:3B:DA:9B:6F:68
"$HOME/.platformio/penv/bin/python" -u .github/skills/esp32-buttonless-flash/scripts/flash_app.py --serial D8:3B:DA:9B:6F:68 --before usb_reset --flash
```

This script writes no bootloader/partition/NVS images, bounds each sector erase
to the existing app partition, identifies the board by serial before each
operation, checks esptool's reported MAC, retries a block at most three times,
and requires an independent whole-image digest match. It waits for macOS `/dev`
re-enumeration events for at most 15 seconds, without sleep/poll loops. Python
3.9 kqueue handles are explicitly closed, not used as context managers.

When a chunk fails, retain the exact image and failure address. A repeatable
resume is available, but only at one of that image's chunk boundaries. The
script independently verifies the skipped prefix BEFORE writing anything:

```sh
"$HOME/.platformio/penv/bin/python" -u .github/skills/esp32-buttonless-flash/scripts/flash_app.py --serial D8:3B:DA:9B:6F:68 --before usb_reset --resume-from 0x58000 --flash
```

The address above is an example from the successful recovery, not a default.
Never skip a prefix based only on a historical log. A mismatch stops the tool.
After any write interruption the application can be incomplete: do not boot or
claim success until the final whole-image digest passes.

### Direct Whole-Image Alternative

Substitute the confirmed ROM port; these example commands use `/dev/cu.usbmodem1101`:

```sh
"$HOME/.platformio/penv/bin/python" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 115200 --no-stub --before no_reset --after no_reset write_flash --flash_size keep 0x10000 .pio/build/esp32s3/firmware.bin
"$HOME/.platformio/penv/bin/python" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 115200 --no-stub --before no_reset --after hard_reset verify_flash 0x10000 .pio/build/esp32s3/firmware.bin
```

Require `Hash of data verified` for the write and independent `verify OK (digest
matched)` for the second command. Check command exit codes and esptool's reported
MAC. The ROM `no_reset` warning is expected when download mode was confirmed.

On write/verify failure, do not claim success or boot a known partial image.
Re-identify the same board/ROM port and retry the same application image only
after understanding the failure. Do not expand erase/write scope.
In the 2026-11-06 run, a whole-image write disconnected at 11%; the app-only
chunk writer plus prefix-verified resume completed successfully. Prefer the
bounded chunk path when this macOS USB disconnect recurs.

## 4. Confirm Application Restart

Re-enumerate ports. `Hard resetting via RTS pin` does not prove a running app.
If the application CDC returns, proceed to logs.
The 2026-11-06 chunked run DID return to application CDC automatically after
independent verification and hard reset, without buttons or a power cycle.
This supersedes neither the historical fallback nor the requirement to check
the observed interface on every run.

If the board remains in ROM, report that flashing and digest verification have
completed but application startup has not. This board's RESET button is broken.
The verified fallback is a user-assisted full power cycle, not pressing RESET
or repeatedly sending reset commands. Disconnect USB and any battery supply
using the normal switch or removable connector, then reconnect. USB removal
alone is insufficient when the battery keeps the chip powered.

Do not promise a fully software-only restart, write speculative reset registers,
bypass esptool safety checks, or erase storage to leave ROM. Ask the user for
the necessary physical power action and then continue verification.

## 5. Monitor Configuration And Sensor Logs

Use the re-enumerated application port at 115200bps, with DTR and RTS both true.
Arduino 2.0.14 `USBCDC` requires those line states to mark CDC connected;
the previously used false/false listener received no firmware logs.

```sh
"$HOME/.platformio/penv/bin/python" -u -c 'import serial, datetime; port = serial.Serial(port=None, baudrate=115200, timeout=1); port.dtr = True; port.rts = True; port.port = "/dev/cu.usbmodemD83BDA9B6F682"; port.open(); print("CDC monitor @115200", flush=True)
while True:
 data = port.readline()
 if data: print(datetime.datetime.now().strftime("%H:%M:%S") + " " + data.decode("utf-8", errors="replace").rstrip(), flush=True)'
```

Use an asynchronous terminal only for this intentional continuous monitor.
Retain relevant current-run logs under `test/results/` when testing hardware.
Stop the monitor before another flash. An open-ended logger is not an input
prompt; do not feed it answers or poll repeatedly without a new event.

Check `[IMU] QMI8658 ready` for board IMU initialization. For the wearable,
distinguish link, GATT discovery/subscription, first decoded angle frame and
continuing fresh data. The verified WT901BLE67 address was
`E7:E0:93:0D:D0:8D`, RANDOM_STATIC/type 1; an app identifier is not necessarily
its advertising MAC. Its vendor service/notify UUIDs end in `5f9a34fb`, not
the standard Bluetooth base `5f9b34fb`.

Repeated `[PWR] power OFF` with disabled shutdown-chime messages means the old
firmware is in its shutdown path while still powered, not a BLE connection error
or a prompt. Report it and recover application operation before sensor testing.

## Report

Report the actual image, device identity, app-only write address, write and
independent verification results, observed application boot/logs, and anything
still unverified. Disclose any required user power cycle. Never equate mocks,
historical logs or a flash digest with current physical sensor/UI validation.