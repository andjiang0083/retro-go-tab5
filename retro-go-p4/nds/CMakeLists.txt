cmake_minimum_required(VERSION 3.5)
# NDS core app。模式完全沿用 gbsp（独立分区，不碰现有 app）。
# 构建：python3 rg_tool.py --target tab5 --no-networking build nds
#   （已在 rg_tool.py PROJECT_APPS 登记 'nds'；DEFAULT_APPS 暂未加入——见 docs/nds/lessons.md）
# M1：显示通路（双屏 256×384）；卡带/BIOS 未接（M5）。
# ── NDS 核心外置（真源在独立项目 tab5-nds；本树只保留 app 壳）────────────────
# 契约见 tab5-nds/integration/contract.md；校验脚本 integration/check_contract.sh
if(NOT DEFINED TAB5_NDS_ROOT)
    set(TAB5_NDS_ROOT "$ENV{TAB5_NDS_ROOT}" CACHE PATH "tab5-nds 项目根（NDS 核心真源）")
endif()
if(NOT TAB5_NDS_ROOT)
    set(TAB5_NDS_ROOT "$ENV{HOME}/esp32/tab5-nds")
endif()
if(NOT EXISTS "${TAB5_NDS_ROOT}/core/arm9-cpu/arm9_cpu.c")
    message(FATAL_ERROR "找不到 NDS 核心：设 TAB5_NDS_ROOT 指向 tab5-nds 项目根（当前 '${TAB5_NDS_ROOT}'）")
endif()
set(EXTRA_COMPONENT_DIRS "${TAB5_NDS_ROOT}/core")
message(STATUS "NDS 核心来自: ${TAB5_NDS_ROOT}/core")

set(COMPONENTS "main retro-go nds-core arm9-cpu app_trace bootloader esptool_py")
include(../base.cmake)
project(nds)
