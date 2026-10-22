#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

action=${1:-all}
image=aeroprobe-idf-test:4.4.6
test_path=/opt/pytest-venv/bin:/opt/esp-qemu/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

case "$action" in
  all|host|build|qemu|hardware|image) ;;
  *) printf '%s\n' 'Usage: sh test/run.sh [all|host|build|qemu|hardware|image]' >&2; exit 2 ;;
esac

if [ "$action" = hardware ]; then
  if [ "$(uname -s)" != Linux ]; then
    printf '%s\n' 'Docker Desktop cannot directly map macOS serial ports. See test/README.md for host pytest instructions.' >&2
    exit 2
  fi
  : "${ESPPORT:?Set ESPPORT to the Linux serial device; this mode flashes test firmware}"
fi

docker build -f test/Dockerfile -t "$image" .
if [ "$action" = image ]; then exit 0; fi
mkdir -p test/results

if [ "$action" = all ] || [ "$action" = host ]; then
  docker run --rm -v "$PWD:/project" "$image" env PATH="$test_path" \
    python -m pytest -q test/host/test_regression.py \
    --root-logdir=/project/test/results/host-logs \
    --junitxml=/project/test/results/host.xml
fi

if [ "$action" != host ]; then
  docker run --rm -v "$PWD:/project" "$image" \
    idf.py -C /project/test/embedded -B /project/test/embedded/build/esp32s3 \
    -DIDF_TARGET=esp32s3 -DSDKCONFIG=/project/test/embedded/build/esp32s3/sdkconfig build
fi

if [ "$action" = all ] || [ "$action" = qemu ]; then
  docker run --rm -v "$PWD:/project" "$image" env PATH="$test_path" \
    python -m pytest -q test/embedded/pytest_runtime.py \
    --embedded-services=idf,qemu --target=esp32s3 \
    --app-path=/project/test/embedded --build-dir=build/esp32s3 \
    --root-logdir=/project/test/results/qemu-logs \
    --junitxml=/project/test/results/unity-qemu.xml
fi

if [ "$action" = hardware ]; then
  docker run --rm --device "$ESPPORT:/dev/ttyDUT" -v "$PWD:/project" "$image" \
    env PATH="$test_path" python -m pytest -q -s test/embedded/pytest_runtime.py \
    --embedded-services=idf,esp --target=esp32s3 --port=/dev/ttyDUT --baud=115200 \
    --app-path=/project/test/embedded --build-dir=build/esp32s3 \
    --root-logdir=/project/test/results/hardware-logs \
    --junitxml=/project/test/results/unity-hardware.xml
fi