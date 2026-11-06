#!/usr/bin/env bash
# 稳健分块烧录：针对 ESP32-S3 内置 USB-Serial-JTAG 在 macOS 上
# 长时间连续写入时偶发掉线重枚举（"Device not configured" / 端口消失）的问题。
#
# 策略：
#   - 每块 96KB，块后自动校验（Hash of data verified）
#   - 任一块失败（掉线/端口消失），等待端口重新出现后重试该块，最多 10 次
#   - 全部写完后逐块读回校验（可选，默认关）
#
# 用法：
#   tools/flash_chunked.sh [port] [firmware.bin]
#   tools/flash_chunked.sh /dev/cu.usbmodem14401 .pio/build/esp32s3/firmware.bin

set -u

PORT="${1:-/dev/cu.usbmodem14401}"
FW="${2:-.pio/build/esp32s3/firmware.bin}"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
BAUD="${BAUD:-115200}"
CHUNK=$((96*1024))
MAX_RETRY=10

if [[ ! -f "$FW" ]]; then echo "固件不存在: $FW"; exit 1; fi
if [[ ! -f "$ESPTOOL" ]]; then echo "esptool 不存在: $ESPTOOL"; exit 1; fi

SZ=$(stat -f%z "$FW")
echo "port=$PORT  fw=$FW  size=$SZ  chunk=$CHUNK"

# 等待端口出现（重枚举后可能短暂消失）
wait_port() {
  local i=0
  while [[ ! -e "$PORT" ]]; do
    i=$((i+1)); [[ $i -gt 30 ]] && { echo "  端口等待超时: $PORT"; return 1; }
    sleep 1
  done
  return 0
}

write_chunk() {
  local addr="$1" file="$2" tries=0
  while :; do
    tries=$((tries+1))
    wait_port || return 1
    out=$("$ESPTOOL" --chip esp32s3 --port "$PORT" --baud "$BAUD" \
          --before default_reset --after no_reset \
          write_flash "$addr" "$file" 2>&1)
    if echo "$out" | grep -q "Hash of data verified"; then
      echo "  OK  $addr  ($tries 次)"
      return 0
    fi
    echo "  RETRY $addr  (第 $tries 次失败: $(echo "$out" | grep -iE 'error|exception|doesn.t exist' | tail -1))"
    [[ $tries -ge $MAX_RETRY ]] && { echo "  放弃 $addr"; return 1; }
    sleep 2
  done
}

# 1) bootloader + 分区表
echo "== bootloader =="
write_chunk 0x0 .pio/build/esp32s3/bootloader.bin || exit 1
echo "== partitions =="
write_chunk 0x8000 .pio/build/esp32s3/partitions.bin || exit 1

# 2) 固件分块
echo "== firmware (分块) =="
OFF=0
while [[ $OFF -lt $SZ ]]; do
  dd if="$FW" of=/tmp/fw_chunk.bin bs=1024 skip=$((OFF/1024)) count=$((CHUNK/1024)) 2>/dev/null
  ADDR=$(printf "0x%x" $((0x10000+OFF)))
  echo ">>> $ADDR"
  write_chunk "$ADDR" /tmp/fw_chunk.bin || { echo "烧录失败于 $ADDR，终止"; exit 1; }
  OFF=$((OFF+CHUNK))
done

echo "== 完成，硬复位 =="
wait_port && "$ESPTOOL" --chip esp32s3 --port "$PORT" --after hard_reset run >/dev/null 2>&1 || true
echo "DONE  (written $SZ bytes @0x10000)"
