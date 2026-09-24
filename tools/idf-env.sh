#!/bin/sh
# Tab5 retro-go 项目统一构建环境
# 用法: . tools/idf-env.sh
#
# 为什么不用现成的 IDF：本机有两份 v5.5 安装，只有 ~/esp/esp-idf-v5.5 (5.5.2) 完整可用
# （~/.espressif/v5.5.4 那份缺 python venv）。且必须显式指定 venv：
# 后台 shell 的 python3 可能指向无关 venv（~/.copaw/venv 3.12），
# 会让 export.sh 选错 python env 并在依赖检查阶段失败。
# 另外 PYTHONPATH 必须清掉，否则污染 IDF 的 python 依赖解析（技能的 8.20 条）。

unset PYTHONPATH
unset IDF_TOOLS_PATH
export IDF_PATH="$HOME/esp/esp-idf-v5.5"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.14_env"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
. "$IDF_PATH/export.sh" >/dev/null 2>&1
