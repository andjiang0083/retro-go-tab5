#!/bin/bash
# 刷 Tab5 + 抓一段启动日志（**串口独占**：刷写与抓日志在同一个后台任务里串起来，
# 绝不让两个任务同时碰 /dev/cu.usbmodem1101）
# 用法：tools/flash-tab5.sh <merged.bin> [日志秒数]
set -o pipefail

IMG="$1"
LOG_SECS="${2:-30}"
[ -z "$IMG" ] && { echo "用法: $0 <merged.bin> [日志秒数]"; exit 2; }

export IDF_PYTHON_ENV_PATH=/Users/jiangweizhong/.espressif/python_env/idf5.5_py3.12_env
source ~/esp/esp-idf-v5.5/export.sh >/dev/null 2>&1
cd ~/esp32/retro-go-tab5 || exit 1

echo "=== FLASH START $(date '+%F %T')  $IMG ==="
# ⚠ 刷写期可能 BOD 欠压复位 + USB 重新枚举：esptool 会抛 OSError/串口消失，
#   当作"设备重启"重试（最多 3 次），不要当成刷写失败。
for attempt in 1 2 3; do
    echo "--- attempt $attempt ---"
    python3 -m esptool --chip esp32p4 -b 921600 --before default_reset --after hard_reset \
        write_flash -z 0x0 "$IMG"
    rc=$?
    echo "--- attempt $attempt rc=$rc ---"
    [ $rc -eq 0 ] && break
    echo "（按设备重启处理，3 秒后重试）"
    sleep 3
done
if [ $rc -ne 0 ]; then
    echo "=== FLASH FAILED rc=$rc $(date '+%F %T') ==="
    exit $rc
fi
echo "=== FLASH OK $(date '+%F %T')，等 4 秒让设备起来 ==="
sleep 4
echo "=== BOOT LOG (${LOG_SECS}s) ==="
sh tools/log-tab5-nr.sh "$LOG_SECS"
echo "=== DONE $(date '+%F %T') ==="
