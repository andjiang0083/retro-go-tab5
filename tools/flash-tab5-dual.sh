#!/usr/bin/env bash
# 一键刷入 Tab5 双方向固件（竖屏保底槽 + 横屏 `_l` 槽）。
#
# 用法：tools/flash-tab5-dual.sh [竖屏merged.img] [横屏三件套目录]
#   默认：dist/orient-r1-portrait/merged-portrait.img  dist/orient-r1-landscape/
#
# 为什么需要这个脚本（而不是直接 flash-tab5.sh）：
#   merged 镜像里的分区表来自 retro-go-p4/rg_tool.py 的 build_image()，它**只声明本趟构建的
#   那几个 app**；横屏槽（`launcher_l` 等）的镜像不在同一趟构建里，所以分区表要单独显式写入。
#   另外 retro-go-p4/partitions.csv 是构建产物、不是真源（ESP32-经验沉淀.md §233），
#   分区表真源是 tools/partitions-dual-tab5.csv。
#
# 顺序很关键：① 先刷竖屏 merged（含 bootloader；它的表只有 3 槽）→ ② 用真源表覆盖 0x8000
#   → ③ 再写横屏三件套到 `_l` 槽偏移。②③ 都不动 nvs/otadata/phy_init（偏移与值完全不变）。
world_src=''
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v5.5}"
IDFPY=$(ls -d "$HOME"/.espressif/python_env/*/bin/python | head -1)
ESPTOOL="$IDF_PATH/components/esptool_py/esptool/esptool.py"
GEN="$IDF_PATH/components/partition_table/gen_esp32part.py"
PORT="${DEV_PORT:-/dev/cu.usbmodem1101}"
BAUD="${FLASH_BAUD:-460800}"

PORTRAIT="${1:-dist/orient-r1-portrait/merged-portrait.img}"
LANDDIR="${2:-dist/orient-r1-landscape}"
CSV="tools/partitions-dual-tab5.csv"
LOG=".buildlogs/flash-dual.log"; mkdir -p .buildlogs

say() { echo "=== $* ==="; }

say "⓪ 分区表一致性门禁（真源 CSV vs rg_tool.py 元组 vs 镜像内嵌表）"
python3 tools/check-dual-partition.py "$PORTRAIT" || { echo "✗ 门禁未通过 —— 拒绝动设备（真源/生成物/镜像三者漂移，见沉淀 §233）"; exit 1; }

say "① 刷前身份断言（§230 纪律）"
for f in "$PORTRAIT" "$LANDDIR/launcher.bin" "$LANDDIR/retro-core.bin" "$LANDDIR/gbsp.bin" "$CSV"; do
    [ -f "$f" ] || { echo "✗ 缺文件：$f"; exit 1; }
    printf '  %-52s %9s B  %s\n' "$f" "$(stat -f%z "$f")" "$(shasum -a256 "$f" | cut -c1-16)…"
done

say "② 刷竖屏整包（含 bootloader；会写入一张只有 3 槽的表，下一步覆盖）"
FLASH_BAUD="$BAUD" FLASH_NO_COMPRESS=1 bash tools/flash-tab5.sh "$PORTRAIT" 20 > "$LOG" 2>&1 || true
grep -qE "Hash of data verified|FLASH OK" "$LOG" && echo "  整包 OK" || { echo "✗ 整包刷写失败，见 $LOG"; exit 1; }

say "③ 生成并写入双方向分区表（真源: ${CSV}）→ 0x8000"
"$IDFPY" "$GEN" "$CSV" .buildlogs/dual-partition-table.bin >/dev/null
"$IDFPY" "$ESPTOOL" -p "$PORT" -b "$BAUD" write_flash 0x8000 .buildlogs/dual-partition-table.bin 2>&1 | grep -E "verified|Error" | tail -2

say "④ 写横屏三件套到 \`_l\` 槽"
"$IDFPY" "$ESPTOOL" -p "$PORT" -b "$BAUD" write_flash \
  0x3A0000 "$LANDDIR/launcher.bin" \
  0x4A0000 "$LANDDIR/retro-core.bin" \
  0x620000 "$LANDDIR/gbsp.bin" 2>&1 | grep -E "Wrote|verified|Error" | tail -6

say "⑤ 读回设备端分区表核对（必须是 6 槽）"
"$IDFPY" "$ESPTOOL" -p "$PORT" -b "$BAUD" read_flash 0x8000 0xC00 .buildlogs/dev-part-final.bin >/dev/null 2>&1
"$IDFPY" "$GEN" .buildlogs/dev-part-final.bin 2>/dev/null | tail -9

say "⑥ 确保引导槽 = launcher（竖屏，用户定的默认）"
export PYTHONPATH="$IDF_PATH/components/partition_table:$IDF_PATH/components/app_update"
"$IDFPY" "$IDF_PATH/components/app_update/otatool.py" --port "$PORT" switch_ota_partition --name launcher 2>&1 | tail -1
"$IDFPY" "$IDF_PATH/components/app_update/otatool.py" --port "$PORT" read_otadata 2>&1 | tail -2

echo
echo "完成。设备下次开机 = 竖屏；boot 里 NVS 无 ui/orient 时首启会弹方向选择页。"
