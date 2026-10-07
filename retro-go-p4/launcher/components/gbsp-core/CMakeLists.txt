# 单 app 形态专用（-DRG_SINGLE_APP=1）：把 gbsp 核心的源码编成本组件，入口改名为
# rg_core_main —— 同一个 app 里不能有两个 app_main（注意 IDF 的入口叫 app_main，不是 main，
# 所以改名要改 app_main）。
# 核心的源码一行都不用改。
#
# 为什么核心能这样"搬进来"：gbsp/main 只有一个 main.c（外加构建期生成的 bios.h），真正的模拟器在
# gbsp-libretro 组件里；两者都不关心自己被谁调用，只管 rg_system_init() 读到的
# NS_BOOT 名字（决定 configNs）和 bootArgs（决定 romPath），这两样在单 app 形态下与双 app
# 形态完全一样 —— 所以核心的行为不需要任何适配。
set(COMPONENT_SRCDIRS "../../../gbsp/main")
set(COMPONENT_ADD_INCLUDEDIRS "../../../gbsp/main" "${CMAKE_CURRENT_BINARY_DIR}")
set(COMPONENT_REQUIRES retro-go gbsp-libretro)
register_component()
rg_setup_compile_options()

set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/../../../gbsp/main/main.c"
    PROPERTIES COMPILE_FLAGS "-Dapp_main=rg_core_main")

# ── P2-5：核心源码现在 `#include "bios.h"`，而这个头是**构建期生成**的（不再提交到仓库）。
# 本组件与 gbsp/main 是同一份源码的另一个宿主（单 app 形态），所以这里也要接一次生成 ——
# 生成产物落在各自的构建目录里，互不干扰。生成器与门禁见 gbsp/main/gen_bios_header.cmake。
set(GBSP_BIOS_BIN "${CMAKE_CURRENT_LIST_DIR}/../../../gbsp/components/gbsp-libretro/bios/open_gba_bios.bin")
set(GBSP_BIOS_HDR "${CMAKE_CURRENT_BINARY_DIR}/bios.h")
add_custom_command(
    OUTPUT  "${GBSP_BIOS_HDR}"
    COMMAND ${CMAKE_COMMAND}
            -DGBSP_BIOS_BIN=${GBSP_BIOS_BIN}
            -DGBSP_BIOS_HDR=${GBSP_BIOS_HDR}
            -P "${CMAKE_CURRENT_LIST_DIR}/../../../gbsp/main/gen_bios_header.cmake"
    DEPENDS "${GBSP_BIOS_BIN}" "${CMAKE_CURRENT_LIST_DIR}/../../../gbsp/main/gen_bios_header.cmake"
    COMMENT "生成 BIOS 兜底头 bios.h（P2-5，gbsp-core 宿）"
    VERBATIM)
add_custom_target(gbsp_core_bios_header DEPENDS "${GBSP_BIOS_HDR}")
add_dependencies(${COMPONENT_LIB} gbsp_core_bios_header)
