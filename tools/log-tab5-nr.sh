#!/bin/sh
# 抓 Tab5 运行中日志（不复位！）
# 用法: sh tools/log-tab5-nr.sh [秒数]
# 与 log-tab5.sh 的区别：不做 DTR/RTS 复位时序，仅打开端口读取，避免打断正在运行的游戏。
#
# 2026-10-08 修：刷机后 USB 会重新枚举，端口短暂消失/改名，旧版开场直接 open 会
#   `OSError: [Errno 6] Device not configured` 立刻退出（真机实测：B 组因此整轮 0 行日志）。
#   现在：等端口出现 → 打开失败/中途掉线都重试重开 → 直到秒数用满。
set -e
SECS="${1:-20}"
PORT="${PORT:-}"
PY="$HOME/.espressif/python_env/idf5.5_py3.14_env/bin/python"

[ -x "$PY" ] || { echo "找不到 IDF 的 python：$PY"; exit 1; }

"$PY" - "$PORT" "$SECS" <<'PYEOF'
import glob, sys, time

port_arg, secs = sys.argv[1], float(sys.argv[2])

try:
    import serial
except ImportError:
    sys.exit("缺 pyserial（请用 IDF 的 python 跑）")


def find_port():
    if port_arg:
        return port_arg
    c = sorted(glob.glob('/dev/cu.usbmodem*'))
    return c[0] if c else None


deadline = time.time() + secs
port, ser, reopens = None, None, 0

while time.time() < deadline:
    if ser is None:
        p = find_port()
        if not p:
            time.sleep(0.5)          # 复位后端口尚未重新枚举出来
            continue
        try:
            ser = serial.Serial(p, 115200, timeout=1)
            # 立刻拉低 DTR/RTS 并保持，不做任何跳变 —— 避免触发 P4 原生 USB 复位
            ser.dtr = False
            ser.rts = False
            time.sleep(0.2)
            if p != port:
                print(f"[log-tab5-nr] 已连上 {p}", flush=True)
                port = p
        except (OSError, serial.SerialException):
            ser = None
            time.sleep(0.5)          # 打开失败（设备还没配好）→ 重试
            continue
    try:
        n = ser.in_waiting
        if n:
            sys.stdout.write(ser.read(n).decode('utf-8', 'replace'))
            sys.stdout.flush()
        else:
            time.sleep(0.05)
    except (OSError, serial.SerialException):
        try:
            ser.close()
        except Exception:
            pass
        ser = None                   # 中途掉线（重新枚举/被别的进程抢）→ 关掉重开
        reopens += 1
        time.sleep(0.5)

if ser is not None:
    ser.close()
if reopens:
    print(f"[log-tab5-nr] 期间端口重连 {reopens} 次", flush=True)
PYEOF
