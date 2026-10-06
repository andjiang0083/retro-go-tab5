#!/bin/sh
# 串口抓取（诊断用）：等端口出现再挂 monitor，避免刷机后端口短暂消失导致脚本直接退出。
OUT="${1:-/tmp/touch_cal2.log}"
cd "$HOME/esp32/retro-go-tab5" || exit 1
rm -f "$OUT"
i=0
while [ ! -e /dev/cu.usbmodem101 ] && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done
[ -e /dev/cu.usbmodem101 ] || { echo "PORT_TIMEOUT" >> "$OUT"; exit 1; }
sleep 2
exec sh tools/log-tab5-nr.sh >> "$OUT" 2>&1
