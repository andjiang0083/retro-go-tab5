#!/bin/bash
# 强制写入 Tab5 **单 app 形态**的方向真值（NVS `rgapp/orient`），用于免 UI 验证方向切换。
#
# 用法:  tools/force-orient-nvs.sh 0|1 [日志秒数]      # 0=竖屏  1=横屏
#        DEV_PORT=/dev/cu.usbmodem101 tools/force-orient-nvs.sh 1 45
#
# 为什么要这个脚本（而不是点设置页）:
#   单 app 形态下"当前方向"的权威值是 NVS，显示初始化**早于**存储层 ⇒ 没法用 SD 上的设置去改。
#   真机验证横屏后端/横屏几何能不能起来，最直接的办法就是把 NVS 写成 1 然后复位。
#   设置页那条路（rg_orient_restart_into）最终也是写同一个 key，所以两者等价。
#
# ⚠ 这会**整块重写** nvs 分区（16K）⇒ 同分区里的其它 NVS 内容（如 `rgapp/core` 待续标志、
#   少量设备设置）会被清掉，设备会重新初始化它们。测试用没问题，别在"用户已存了很多设置"的
#   机器上随手跑。存档在 SD 上，不受影响。
#
# 原理: $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py
#       generate <csv> <out.bin> 0x4000   →   esptool write_flash 0x9000 <out.bin>
set -e

VALUE="$1"
LOG_SECS="${2:-45}"
[ -z "$VALUE" ] && { echo "用法: $0 0|1 [日志秒数]   (0=竖屏 1=横屏)"; exit 2; }
case "$VALUE" in 0|1) ;; *) echo "❌ 只接受 0 或 1"; exit 2;; esac

: "${DEV_PORT:=/dev/cu.usbmodem1101}"   # ⚠ 本机是 /dev/cu.usbmodem101；脚本默认值偏保守
: "${IDF_ENV_SH:=$HOME/esp/esp-idf-v5.5/export.sh}"
: "${IDF_PATH:=$HOME/esp/esp-idf-v5.5}"
: "${DEV_TREE:=$(cd "$(dirname "$0")/.." && pwd)}"
: "${LOG:=.buildlogs/force-orient-$VALUE.log}"

mkdir -p "$(dirname "$LOG")"
WORK="$(mktemp -d)"
CSV="$WORK/orient.csv"
BIN="$WORK/nvs.bin"

cat > "$CSV" <<EOF
key,type,encoding,value
rgapp,namespace,,
orient,data,u8,$VALUE
EOF

# shellcheck disable=SC1090
source "$IDF_ENV_SH" >/dev/null 2>&1 || true
PY="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/python_env/idf5.5_py3.12_env}/bin/python"
[ -x "$PY" ] || PY="$(command -v python3)"
GEN="$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
[ -f "$GEN" ] || { echo "❌ 找不到 nvs_partition_gen.py: $GEN"; exit 3; }

echo "▶ 生成 nvs 镜像: rgapp/orient = $VALUE"
"$PY" "$GEN" generate "$CSV" "$BIN" 0x4000

echo "▶ 刷到 0x9000（nvs 分区，16K），随后复位并抓 $LOG_SECS 秒日志"
{
  echo "=== FORCE ORIENT $VALUE  $(date '+%Y-%m-%d %H:%M:%S')  port=$DEV_PORT ==="
  "$PY" -m esptool --chip esp32p4 --port "$DEV_PORT" -b 460800 \
      write_flash --flash_mode dio --flash_size 16MB 0x9000 "$BIN"
  sleep 1
  "$PY" -m esptool --chip esp32p4 --port "$DEV_PORT" run
  "$PY" - "$DEV_PORT" "$LOG_SECS" <<'PYEOF'
import sys, time, serial
port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.2)
end = time.time() + secs
while time.time() < end:
    line = s.readline()
    if line:
        sys.stdout.write(line.decode("utf-8", "replace").rstrip() + "\n")
        sys.stdout.flush()
s.close()
PYEOF
} > "$LOG" 2>&1
echo "rc=$?  日志: $LOG"

echo
echo "=== 判据（两方向各跑一次对照）==="
echo "  竖屏应出现: logical 720x1280 -> physical 720x1280 (linear 1:1 map)   + rows=32"
echo "  横屏应出现: 90CW map（或等价旋转映射）                              + rows=16"
grep -m3 -E "linear 1:1 map|90CW map|rows=" "$LOG" || true
rm -rf "$WORK"
