include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
# retro-go-tab5: 复用仓库级 vendor/（M5Stack 官方 Tab5 BSP + 其离线依赖，与自检固件共享一份）
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../vendor"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/managed_components")

macro(rg_setup_compile_options)
    component_compile_options(
        -D${RG_BUILD_TARGET}=1
        -DRETRO_GO=1
        -fjump-tables -ftree-switch-conversion
        ${ARGV}
    )

    if(RG_ENABLE_NETPLAY)
        component_compile_options(-DRG_ENABLE_NETWORKING -DRG_ENABLE_NETPLAY)
    elseif(RG_ENABLE_NETWORKING)
        component_compile_options(-DRG_ENABLE_NETWORKING)
    endif()

    if(RG_ENABLE_PROFILING)
        # Still debating whether -fno-inline is necessary or not...
        component_compile_options(-DRG_ENABLE_PROFILING -finstrument-functions)
    endif()

    if(RG_SINGLE_APP)
        # 单 app 形态：菜单与模拟器核心编进同一个 app 镜像。
        # 用途：M5Launcher 这类"只装一个 app 镜像"的启动器装进来也能玩（双 app 形态下它只装
        # 第一个 app，核心装不进去 → 游戏起不来）。实现见 launcher/components/gbsp-core 与
        # components/retro-go/rg_system.c 的 RG_SINGLE_APP 分支。
        # 构建：idf.py -DRG_SINGLE_APP=1 ...
        # RG_SINGLE_APP_CORE 必须与 launcher/CMakeLists.txt 里加进来的核心组件一致
        # （它决定菜单认定"哪些核心已安装" —— 见 rg_system_have_app）。
        component_compile_options(-DRG_SINGLE_APP=1 -DRG_SINGLE_APP_CORE=\"gbsp\")
    endif()
endmacro()
