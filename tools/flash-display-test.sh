#!/bin/sh
# 刷 display-test 自检固件到 Tab5
# 用法: sh tools/flash-display-test.sh
# 注意：首次刷 bootloader 必须 --force（技能 8.23：rev1.3 与 bootloader 镜像头的 rev 校验）
# 判成功：esptool 输出 Hash of data verified + 回读校验，不看末尾 tail
set -e
PORT="${PORT:-/dev/cu.usbmodem101}"
PROJ="$HOME/esp32/retro-go-tab5/display-test"
PY="$HOME/.espressif/python_env/idf5.5_py3.14_env/bin/python"

cd "$PROJ/build"
echo "=== 刷入前先核对设备分区表（技能 8.58）==="
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0x8000 0xC00 /tmp/dev_pt_now.bin
cmp -s /tmp/dev_pt_now.bin partition_table/partition-table.bin \
  && echo "OK: 设备分区表与本构建一致" \
  || echo "WARN: 分区表不同（本固件会重写它，已备份则无妨）"

echo "=== 写入 bootloader + 分区表 + app ==="
# esptool 4.12 参数用下划线（default-reset 会报 invalid choice）
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 \
  --before default_reset --after hard_reset \
  write_flash --verify --force @flash_args

echo "=== 回读校验：app 头 64B（与本地前 64B 比）+ bootloader 整段官方校验 ==="
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0x10000 0x40 /tmp/dev_app_after.bin
head -c 64 tab5-display-test.bin | shasum -a 256
shasum -a 256 /tmp/dev_app_after.bin
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 verify_flash --flash_mode dio --flash_size 16MB \
  0x2000 bootloader/bootloader.bin 2>&1 | grep -E "verify OK|Verifying"
