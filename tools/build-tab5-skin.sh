#!/bin/bash
# 构建 Tab5 双形态（多 app 常规包 + 单 app M5Launcher 包）
# 用法：tools/build-tab5-skin.sh [single-app]
# ⚠ 长任务：本脚本必须在后台跑、日志落盘（用户硬约束：别在前台/管道 tail）
set -o pipefail

export IDF_PYTHON_ENV_PATH=/Users/jiangweizhong/.espressif/python_env/idf5.5_py3.12_env
source ~/esp/esp-idf-v5.5/export.sh >/dev/null 2>&1

cd ~/esp32/retro-go-tab5/retro-go-p4 || exit 1

LOGDIR=~/esp32/retro-go-tab5/.buildlogs
mkdir -p "$LOGDIR"

echo "=== BUILD START $(date '+%F %T') mode=${1:-release} ==="
# 显式列出要编的 app：launcher(菜单) + gbsp(GBA) + retro-core(GB/GBC/NES/SMS/GG/SNES/PCE/Lynx/GW)。
# ⚠ 必须显式传：默认的 DEFAULT_APPS 列表在本机环境下会把 retro-core 漏掉（只编出 launcher+gbsp），
#   而启动器的 gb/gbc/nes 条目指向的就是 retro-core 分区 —— 分区不存在 → 判定"不可用" → 入口不显示。
if [ "$1" = "single-app" ]; then
    python3 rg_tool.py --target tab5 --no-networking --single-app release launcher gbsp
else
    python3 rg_tool.py --target tab5 --no-networking release launcher gbsp retro-core
fi
RC=$?
echo "=== BUILD END rc=$RC $(date '+%F %T') ==="
exit $RC
