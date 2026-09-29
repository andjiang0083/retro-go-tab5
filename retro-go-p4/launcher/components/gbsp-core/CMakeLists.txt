# 单 app 形态专用（-DRG_SINGLE_APP=1）：把 gbsp 核心的源码编成本组件，入口改名为
# rg_core_main —— 同一个 app 里不能有两个 app_main（注意 IDF 的入口叫 app_main，不是 main，
# 所以改名要改 app_main）。
# 核心的源码一行都不用改。
#
# 为什么核心能这样"搬进来"：gbsp/main 只有一个 main.c（外加 bios.h），真正的模拟器在
# gbsp-libretro 组件里；两者都不关心自己被谁调用，只管 rg_system_init() 读到的
# NS_BOOT 名字（决定 configNs）和 bootArgs（决定 romPath），这两样在单 app 形态下与双 app
# 形态完全一样 —— 所以核心的行为不需要任何适配。
set(COMPONENT_SRCDIRS "../../../gbsp/main")
set(COMPONENT_ADD_INCLUDEDIRS "../../../gbsp/main")
set(COMPONENT_REQUIRES retro-go gbsp-libretro)
register_component()
rg_setup_compile_options()

set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/../../../gbsp/main/main.c"
    PROPERTIES COMPILE_FLAGS "-Dapp_main=rg_core_main")
