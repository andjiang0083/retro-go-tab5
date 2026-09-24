#!/bin/sh
# 刷 Tab5 前的一次性备份：整片 16MB + NVS + 当前分区表，全部带 SHA256
# 用法: sh tools/backup-tab5.sh [输出目录]
# 目的：刷自检固件/retro-go 之前留下可回退的基线（技能 8.58：改整片镜像类设备必记）
set -e
PORT="${PORT:-/dev/cu.usbmodem101}"
OUT="${1:-$HOME/esp32/retro-go-tab5/backup}"
VENV="$HOME/.espressif/python_env/idf5.5_py3.14_env"
PY="$VENV/bin/python"
STAMP=$(date +%Y%m%d-%H%M%S)
DIR="$OUT/$STAMP"
mkdir -p "$DIR"

run() { echo "+ $*"; "$@"; }

echo "=== 备份 Tab5 @ $PORT -> $DIR ==="
run "$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0x8000 0xC00 "$DIR/partition_table_before.bin"
run "$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0x9000 0xE000 "$DIR/nvs_before.bin"
echo "=== 整片 16MB（约 3-5 分钟）==="
run "$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0 0x1000000 "$DIR/full_16MB_before.bin"

cd "$DIR"
shasum -a 256 ./*.bin > SHA256SUMS.txt
echo "=== 备份完成 ==="
ls -la
cat SHA256SUMS.txt
