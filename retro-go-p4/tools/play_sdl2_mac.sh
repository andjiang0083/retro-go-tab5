#!/bin/sh
# 一键在 Mac 上跑 GBA 游戏（retro-go 的 SDL2 宿主模拟器，不刷机）
#
# 用法: sh tools/play_sdl2_mac.sh /绝对路径/game.gba
#       sh tools/play_sdl2_mac.sh                 # 用上次玩的那个
#
# 说明（为什么这么绕）：
#  - SDL2 宿主版的 ROM 路径【不走 argv】，而是从 NVS 设置读：app.romPath = bootArgs ←
#    sd/retro-go/config/boot.json。且 SDL2 的 rg_system_restart() 就是 exit(1)，
#    所以 launcher 选完游戏只会写这个文件然后退出，得再启一次模拟器进程才进游戏。
#  - 键盘：方向键=移动  X=A键  Z=B键  S=X  A=Y  Q=L  W=R  空格=START  0=SELECT  ESC=菜单  TAB=选项
#  - 判成功：进程持续满核（~100% CPU）且没有新的 ~/Library/Logs/DiagnosticReports/gbsp-*.ips 崩溃报告。
#    模拟器报错走 RG_PANIC→abort，而消息会被 stdout 缓冲吞掉，只能看崩溃报告。
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"
ROM="$1"
BOOT_CONF="$ROOT/sd/retro-go/config/boot.json"

[ -x "$ROOT/build-sdl2/gbsp" ] || { echo "→ 先构建 SDL2 宿主版"; sh tools/build_sdl2_mac.sh; }

if [ -z "$ROM" ] && [ -f "$BOOT_CONF" ]; then
    ROM=$(sed -nE 's/.*"BootArgs":"([^"]*)".*/\1/p' "$BOOT_CONF")
    echo "→ 沿用上次的 ROM: $ROM"
fi
[ -f "$ROM" ] || { echo "用法: sh tools/play_sdl2_mac.sh /绝对路径/game.gba"; exit 1; }

# 转成绝对路径（模拟器按自己的 cwd 解析）
case "$ROM" in /*) ;; *) ROM="$PWD/$ROM";; esac

mkdir -p "$ROOT/sd/retro-go/config"
printf '{"BootName":"gbsp","BootArgs":"%s","BootFlags":0}\n' "$ROM" > "$BOOT_CONF"
echo "→ 启动配置: $(cat "$BOOT_CONF")"
echo "→ 按键: 方向键 移动 | X=A | Z=B | S=X | A=Y | Q=L | W=R | 空格=START | 0=SELECT | ESC=菜单 | TAB=选项"
echo "→ 启动 gbsp（关掉窗口即退出）"
exec "$ROOT/build-sdl2/gbsp"
