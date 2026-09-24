#!/bin/sh
# 抓 Tab5 运行中日志（不复位！）
# 用法: sh tools/log-tab5-nr.sh [秒数]
# 与 log-tab5.sh 的区别：不做 DTR/RTS 复位时序，仅打开端口读取，避免打断正在运行的游戏。
set -e
SECS="${1:-20}"
PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "未找到 /dev/cu.usbmodem* 设备"; exit 1; }
PY="$HOME/.espressif/python_env/idf5.5_py3.14_env/bin/python"

"$PY" - "$PORT" "$SECS" <<'PYEOF'
import serial, sys, time
port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=1)
# 立刻拉低 DTR/RTS 并保持，不做任何跳变 —— 避免触发 P4 原生 USB 复位
s.dtr = False
s.rts = False
time.sleep(0.2)
t0 = time.time()
while time.time() - t0 < secs:
    if s.in_waiting:
        sys.stdout.write(s.read(s.in_waiting).decode('utf-8', 'replace'))
        sys.stdout.flush()
    else:
        time.sleep(0.05)
PYEOF
