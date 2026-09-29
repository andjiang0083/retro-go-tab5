cmake_minimum_required(VERSION 3.5)
set(COMPONENTS "main retro-go wifi esp_http_server")
include(../base.cmake)
if(RG_SINGLE_APP)
    # 单 app 形态（-DRG_SINGLE_APP=1）：把 gbsp 核心也编进本 app，main 则变成"分发器"。
    # 双 app 形态下这些都不参与 —— 核心仍住在自己的 ota_1 分区里（launcher 分区只有
    # 960KB，塞进 1.36MB 的核心会直接溢出）。
    # ⚠ 必须在 include(../base.cmake) 之后：那里会重写 EXTRA_COMPONENT_DIRS。
    # gbsp-libretro 是 gbsp 工程自带的组件，默认不在本工程的搜索路径里。
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../gbsp/components")
    list(APPEND COMPONENTS gbsp-core gbsp-libretro)
endif()
project(launcher)
