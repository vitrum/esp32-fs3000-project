#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
binary=$(mktemp "${TMPDIR:-/tmp}/aeroprobe-host-test.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -pthread -DARDUINO=10800 \
  -Itest/host/mocks -Isrc -I.pio/libdeps/esp32s3/TinyGPSPlus/src \
  test/host/concurrency_test.cpp src/gps_time.cpp \
  .pio/libdeps/esp32s3/TinyGPSPlus/src/TinyGPS++.cpp \
  -o "$binary"
"$binary"
