#!/bin/sh
# 抓 Tab5 启动日志（复位后读 N 秒）
# 用法: sh tools/log-tab5.sh [秒数]   默认 12 秒
#
# 说明：不用 idf.py monitor（要交互），直接用 pyserial 复位+读。
# DTR/RTS 时序是 P4 原生 USB 复位的正确姿势（见 retro-go-porting-p4.md）。
set -e
SECS="${1:-12}"
# 端口自动探测：Tab5 真断电重插后设备号会变（usbmodem101 ↔ usbmodem1101），写死会失联
PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "未找到 /dev/cu.usbmodem* 设备，请检查 USB 连接"; exit 1; }
PY="$HOME/.espressif/python_env/idf5.5_py3.14_env/bin/python"

"$PY" - "$PORT" "$SECS" <<'PYEOF'
import serial, sys, time
port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=1)
time.sleep(0.1)
s.dtr = False; s.rts = True; time.sleep(0.15)
s.dtr = True;  s.rts = False; time.sleep(1.5)
t0 = time.time()
while time.time() - t0 < secs:
    if s.in_waiting:
        sys.stdout.write(s.read(s.in_waiting).decode('utf-8', 'replace'))
        sys.stdout.flush()
    else:
        time.sleep(0.05)
PYEOF
