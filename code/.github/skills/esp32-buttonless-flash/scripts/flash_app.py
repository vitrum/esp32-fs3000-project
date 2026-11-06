"""Plan or explicitly flash only the existing ESP32-S3 application partition."""

import argparse
import csv
import hashlib
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import time

from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[4]
CHUNK_SIZE = 96 * 1024
SECTOR_SIZE = 4096


def normalized_serial(value):
    return re.sub(r"[^0-9a-f]", "", value.lower())


def app_partition(path):
    with path.open(newline="") as stream:
        rows = [list(map(str.strip, row)) for row in csv.reader(stream)]
    apps = [row for row in rows if len(row) >= 5 and row[1] == "app"]
    if len(apps) != 1:
        raise ValueError("Expected exactly one application partition")
    offset, capacity = int(apps[0][3], 0), int(apps[0][4], 0)
    if offset != 0x10000 or capacity != 0x300000:
        raise ValueError("Partition layout changed; separate migration authorization is required")
    return offset, capacity


def chunk_plan(size, offset, capacity):
    if size <= 0 or size > capacity or offset % SECTOR_SIZE:
        raise ValueError("Invalid image size or application boundary")
    plan = []
    for position in range(0, size, CHUNK_SIZE):
        length = min(CHUNK_SIZE, size - position)
        erased_end = offset + position + ((length + SECTOR_SIZE - 1) // SECTOR_SIZE) * SECTOR_SIZE
        if erased_end > offset + capacity:
            raise ValueError("Chunk erase would leave the application partition")
        plan.append((offset + position, position, length))
    return plan


def matching_ports(serial_number):
    return [port for port in list_ports.comports()
            if port.vid == 0x303A and port.pid == 0x1001
            and normalized_serial(port.serial_number or "") == serial_number]


def remaining_plan(plan, resume):
    if resume not in [address for address, _, _ in plan]:
        raise ValueError("Resume must name a chunk boundary in this exact image")
    return [chunk for chunk in plan if chunk[0] >= resume]


def rom_port(serial_number):
    matches = matching_ports(serial_number)
    if not matches and sys.platform == "darwin":
        print("Waiting for USB re-enumeration events (maximum 15 seconds)", flush=True)
        descriptor = os.open("/dev", os.O_RDONLY)
        try:
            events = select.kqueue()
            try:
                watch = select.kevent(descriptor, filter=select.KQ_FILTER_VNODE,
                    flags=select.KQ_EV_ADD | select.KQ_EV_ENABLE | select.KQ_EV_CLEAR,
                    fflags=select.KQ_NOTE_WRITE)
                events.control([watch], 0, 0)
                deadline = time.monotonic() + 15
                matches = matching_ports(serial_number)
                while not matches:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not events.control(None, 1, remaining):
                        break
                    matches = matching_ports(serial_number)
            finally:
                events.close()
        finally:
            os.close(descriptor)
    if len(matches) != 1:
        raise ValueError("Expected exactly one matching USB board; re-enumerate/reconnect before retrying")
    port = matches[0]
    if ":" not in (port.serial_number or ""):
        raise ValueError("Application CDC is still present; perform the authorized 1200bps trigger first")
    return port.device


def execute(esptool, serial_number, before, operation, after="no_reset"):
    port = rom_port(serial_number)
    command = [sys.executable, str(esptool), "--chip", "esp32s3", "--port", port,
               "--baud", "115200", "--no-stub", "--before", before, "--after", after]
    result = subprocess.run(command + operation, capture_output=True, text=True, timeout=120)
    text = result.stdout + result.stderr
    print(text, end="", flush=True)
    return result.returncode, text


def successful(code, text, marker, serial_number):
    macs = re.findall(r"MAC:\s*([0-9a-fA-F:]{17})", text)
    return code == 0 and marker in text and bool(macs) and all(
        normalized_serial(mac) == serial_number for mac in macs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="Confirmed board MAC/USB serial, not a port suffix")
    parser.add_argument("--image", type=Path, default=ROOT / ".pio/build/esp32s3/firmware.bin")
    parser.add_argument("--before", choices=("usb_reset", "no_reset"), default="usb_reset")
    parser.add_argument("--resume-from", type=lambda value: int(value, 0),
                        help="Resume at a chunk address, after independently verifying the skipped prefix")
    parser.add_argument("--flash", action="store_true", help="Actually write after explicit user authorization")
    args = parser.parse_args()
    serial_number = normalized_serial(args.serial)
    if len(serial_number) != 12:
        parser.error("Expected a 12-hex-digit MAC/USB serial")
    image = args.image.resolve()
    data = image.read_bytes()
    if not data or data[0] != 0xE9:
        parser.error("Not an ESP application image")
    offset, capacity = app_partition(ROOT / "partitions.csv")
    plan = chunk_plan(len(data), offset, capacity)
    if args.resume_from is not None:
        plan = remaining_plan(plan, args.resume_from)
    print(f"Image: {image}\nBytes: {len(data)}\nSHA256: {hashlib.sha256(data).hexdigest()}", flush=True)
    for address, _, length in plan:
        print(f"PLAN app-only {address:#x} bytes={length}", flush=True)
    if not args.flash:
        print("DRY RUN: no serial port opened and no device writes performed")
        return
    esptool = Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py"
    if not esptool.is_file():
        parser.error("Existing project esptool was not found")
    with tempfile.TemporaryDirectory(prefix="aeroprobe-app-") as temporary:
        if args.resume_from is not None and args.resume_from > offset:
            prefix = Path(temporary) / "verified-prefix.bin"
            prefix.write_bytes(data[:args.resume_from - offset])
            code, text = execute(esptool, serial_number, args.before,
                                 ["verify_flash", hex(offset), str(prefix)])
            if not successful(code, text, "verify OK (digest matched)", serial_number):
                raise RuntimeError("Resume prefix does not match this image; no chunks were skipped or written")
        for address, position, length in plan:
            chunk = Path(temporary) / f"{address:x}.bin"
            chunk.write_bytes(data[position:position + length])
            for attempt in range(1, 4):
                print(f"WRITE {address:#x} attempt={attempt}/3", flush=True)
                try:
                    code, text = execute(esptool, serial_number, args.before,
                        ["write_flash", "--flash_size", "keep", hex(address), str(chunk)])
                except (ValueError, subprocess.TimeoutExpired) as error:
                    print(f"Chunk interrupted: {error}", flush=True)
                    code, text = 1, ""
                if successful(code, text, "Hash of data verified", serial_number):
                    break
            else:
                raise RuntimeError(f"Stopped at {address:#x}; application is incomplete. Do not boot it")
        code, text = execute(esptool, serial_number, args.before,
                             ["verify_flash", hex(offset), str(image)], after="hard_reset")
        if not successful(code, text, "verify OK (digest matched)", serial_number):
            raise RuntimeError("Whole-image independent verification failed; do not claim success")
    print("SUCCESS: application chunks and independent whole-image digest verified; confirm boot separately", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FLASH FAILED: {error}", file=sys.stderr)
        sys.exit(1)