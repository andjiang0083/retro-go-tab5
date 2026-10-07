set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS "." "${CMAKE_CURRENT_BINARY_DIR}")
register_component()
rg_setup_compile_options()

# ── P2-5：BIOS 兜底数组改为**构建期生成**，仓库里不再提交 gbsp/main/bios.h ───────────
# 为什么：同一份 16 KB BIOS 过去在仓库里存了两份（open_gba_bios.bin 与手写的 bios.h），
# 两份必须逐字节相等却没有任何机制保证 —— 典型的"会静默漂移"的重复。现在唯一事实来源是
# bios/open_gba_bios.bin（它自己由 bios/source/ + Makefile 生成），头文件每次构建现生成。
# 生成器自带 md5 门禁（pin 住 1876f71b…），源漂移即构建失败；细节见 gen_bios_header.cmake。
set(GBSP_BIOS_BIN "${CMAKE_CURRENT_LIST_DIR}/../components/gbsp-libretro/bios/open_gba_bios.bin")
set(GBSP_BIOS_HDR "${CMAKE_CURRENT_BINARY_DIR}/bios.h")
add_custom_command(
    OUTPUT  "${GBSP_BIOS_HDR}"
    COMMAND ${CMAKE_COMMAND}
            -DGBSP_BIOS_BIN=${GBSP_BIOS_BIN}
            -DGBSP_BIOS_HDR=${GBSP_BIOS_HDR}
            -P "${CMAKE_CURRENT_LIST_DIR}/gen_bios_header.cmake"
    DEPENDS "${GBSP_BIOS_BIN}" "${CMAKE_CURRENT_LIST_DIR}/gen_bios_header.cmake"
    COMMENT "生成 BIOS 兜底头 bios.h（P2-5：源自 open_gba_bios.bin，md5 门禁）"
    VERBATIM)
add_custom_target(gbsp_bios_header DEPENDS "${GBSP_BIOS_HDR}")
add_dependencies(${COMPONENT_LIB} gbsp_bios_header)
