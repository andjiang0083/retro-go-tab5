#!/bin/bash
# 刷 Tab5 + 抓一段启动日志（**串口独占**：刷写与抓日志在同一个后台任务里串起来，
# 绝不让两个任务同时碰 /dev/cu.usbmodem1101）
# 用法：tools/flash-tab5.sh <merged.bin> [日志秒数]
set -o pipefail

IMG="$1"
LOG_SECS="${2:-30}"
[ -z "$IMG" ] && { echo "用法: $0 <merged.bin> [日志秒数]"; exit 2; }

# ⚠ 不要在这里写死个人路径 —— 别人机器上克隆下来会直接跑不起来。
# 可覆盖的环境变量：IDF_ENV_SH（ESP-IDF 的 export.sh，默认 $HOME/esp/esp-idf-v5.5/export.sh）、
#                  DEV_TREE（仓库根，默认 = 本脚本所在目录的上一级）、IDF_PYTHON_ENV_PATH（可选）。
: "${DEV_PORT:=/dev/cu.usbmodem1101}"   # 显式端口：esptool 自动选会抓到 /dev/cu.debug-console，刷不上
# 刷写参数可覆盖 —— USB-Serial/JTAG 偶发
#   "A fatal error occurred: Failed to leave compressed flash mode (result was C900: Too much data)"
# （握手阶段就失败，**一个字节都没写进 flash**，设备状态不会被改坏）。降速+关压缩是标准兜底：
#   FLASH_BAUD=460800 FLASH_NO_COMPRESS=1 tools/flash-tab5.sh <merged.bin> [日志秒数]
: "${FLASH_BAUD:=921600}"
: "${FLASH_NO_COMPRESS:=}"              # 非空 = 用 --no-compress（esptool 5.x 的 -u）
COMPRESS_FLAG="-z"
[ -n "$FLASH_NO_COMPRESS" ] && COMPRESS_FLAG="--no-compress"
: "${IDF_ENV_SH:=$HOME/esp/esp-idf-v5.5/export.sh}"
: "${DEV_TREE:=$(cd "$(dirname "$0")/.." && pwd)}"
if [ -n "${IDF_PYTHON_ENV_PATH:-}" ]; then export IDF_PYTHON_ENV_PATH; fi
. "$IDF_ENV_SH" >/dev/null 2>&1
cd "$DEV_TREE" || exit 1

echo "=== FLASH START $(date '+%F %T')  $IMG ==="
# ⚠ 刷写期可能 BOD 欠压复位 + USB 重新枚举：esptool 会抛 OSError/串口消失，
#   当作"设备重启"重试（最多 3 次），不要当成刷写失败。
for attempt in 1 2 3; do
    echo "--- attempt $attempt ---"
    python3 -m esptool --chip esp32p4 -p "$DEV_PORT" -b "$FLASH_BAUD" --before default_reset --after hard_reset \
        write_flash $COMPRESS_FLAG 0x0 "$IMG"
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
