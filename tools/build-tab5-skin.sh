#!/bin/bash
# 构建 Tab5 双形态（多 app 常规包 + 单 app M5Launcher 包）
# 用法：tools/build-tab5-skin.sh [single-app]
#       RG_TAB5_ORIENTATION=1 tools/build-tab5-skin.sh    # 出横屏镜像（默认 0 = 竖屏）
# ⚠ 长任务：本脚本必须在后台跑、日志落盘（用户硬约束：别在前台/管道 tail）
set -o pipefail
# 屏幕方向：0 = 竖屏（默认，= 现状）1 = 横屏。显式 export 给 rg_tool.py（它会每次都传 -D）。
export RG_TAB5_ORIENTATION="${RG_TAB5_ORIENTATION:-0}"
# PPA 传输模式实验开关（只影响横屏驱动）：0 = PPA 关（默认）；1 = 非阻塞；2 = BLOCKING 对照。
# 同样显式 export（rg_tool.py 每次都传 -D），并把档位打进构建日志 —— 实验轮次必须可自证。
export RG_TAB5_PPA_MODE="${RG_TAB5_PPA_MODE:-0}"

# ⚠ 不要在这里写死个人路径 —— 别人机器上克隆下来会直接跑不起来。
# 需要覆盖时用环境变量（默认值 = 本机开发环境）：
#   IDF_ENV_SH           ESP-IDF 的 export.sh（默认 $HOME/esp/esp-idf-v5.5/export.sh，即 5.5.2）
#   DEV_TREE             仓库根（默认 = 本脚本所在目录的上一级，自包含）
#   IDF_PYTHON_ENV_PATH  想让构建用某个 python 环境时再设（不设就交给 export.sh 决定）
: "${IDF_ENV_SH:=$HOME/esp/esp-idf-v5.5/export.sh}"
: "${DEV_TREE:=$(cd "$(dirname "$0")/.." && pwd)}"
if [ -n "${IDF_PYTHON_ENV_PATH:-}" ]; then export IDF_PYTHON_ENV_PATH; fi
. "$IDF_ENV_SH" >/dev/null 2>&1

cd "$DEV_TREE/retro-go-p4" || exit 1

LOGDIR="$DEV_TREE/.buildlogs"
mkdir -p "$LOGDIR"

echo "=== BUILD START $(date '+%F %T') mode=${1:-release} orient=${RG_TAB5_ORIENTATION} ppa=${RG_TAB5_PPA_MODE} ==="
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
