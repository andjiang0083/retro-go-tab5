#!/bin/sh
# 串口抓取（循环版）：retro-go 切换 app 是**软复位**，USB-CDC 端口会消失一下，
# 一次性挂的 monitor 会在那一刻退出 —— 上一轮 8 下就是这么丢的。
# 这个版本掉线就重连，一直录到被杀掉为止。OUT 用 >> 追加，跨重连累积。
OUT="${1:-/tmp/touch_loop.log}"
cd "$HOME/esp32/retro-go-tab5" || exit 1
fails=0
while :; do
    i=0
    while [ ! -e /dev/cu.usbmodem101 ] && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done
    sh tools/log-tab5-nr.sh >> "$OUT" 2>&1
    fails=$((fails+1))
    sleep 2
    if [ $fails -gt 40 ]; then
        sleep 10   # 端口长时间不在（拔线/刷机），降频，别空转
    fi
done
