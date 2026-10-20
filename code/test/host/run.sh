#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
binary=$(mktemp "${TMPDIR:-/tmp}/aeroprobe-host-test.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -pthread -DARDUINO=10800 \
  -Itest/host/mocks -Isrc -I.pio/libdeps/esp32s3/TinyGPSPlus/src \
  test/host/concurrency_test.cpp src/gps_time.cpp src/runtime_log.cpp \
  .pio/libdeps/esp32s3/TinyGPSPlus/src/TinyGPS++.cpp \
  -o "$binary"
"$binary"
"${CXX:-c++}" -std=c++17 -pthread \
  -Itest/host/mocks -Isrc \
  test/host/ble_diagnostics_test.cpp src/runtime_log.cpp src/ble_diagnostics.cpp \
  -o "$binary"
"$binary"
