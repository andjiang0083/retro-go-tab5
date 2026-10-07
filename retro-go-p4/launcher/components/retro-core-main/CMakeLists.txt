# 单 app 形态专用（-DRG_SINGLE_APP=1）：把 retro-core 的 main 编成本组件，入口改名
# rg_core_main_multi —— 与 gbsp 的 rg_core_main 区分（同一个 app 里不能有两个同名入口，
# 而两个核心各自的 main.c 都叫 app_main）。
#
# 为什么只需要搬这一个 main：retro-core/main/main.c 本身就是**分发器** —— 它先跑
# rg_system_init()（按 NS_BOOT 里的名字取到自己的 configNs 与 romPath，和双 app 形态完全一样），
# 再按 configNs 调用对应核心的 xxx_main()：gb/gbc→gbc_main、nes→nes_main、pce→pce_main、
# sms/gg/col→sms_main、gw→gw_main、snes→snes_main、lnx→lynx_main。
# 各核心是独立组件（retro-core/components/ 下），由 COMPONENT_REQUIRES 拉进来。
# 核心源码一行都不用改 —— 同 launcher/components/gbsp-core 的理由。
#
# ⚠ COMPONENT_REQUIRES 里列的核心组件来自 launcher/CMakeLists.txt 追加的
#   EXTRA_COMPONENT_DIRS（逐个指向 retro-core/components/<核心>，刻意避开那里叫
#   "launcher" 的同名组件）。
set(COMPONENT_SRCDIRS "../../../retro-core/main")
set(COMPONENT_ADD_INCLUDEDIRS "../../../retro-core/main")
set(COMPONENT_REQUIRES retro-go gnuboy nofrendo pce-go gw-emulator handy smsplus snes9x)
register_component()
rg_setup_compile_options()

# 入口改名：核心的 app_main 变成 rg_core_main_multi，由 launcher/main/main.c 分发时调用。
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/../../../retro-core/main/main.c"
    PROPERTIES COMPILE_FLAGS "-Dapp_main=rg_core_main_multi")
