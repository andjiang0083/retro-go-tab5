include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
# retro-go-tab5: 复用仓库级 vendor/（M5Stack 官方 Tab5 BSP + 其离线依赖，与自检固件共享一份）
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../vendor"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/managed_components")

macro(rg_setup_compile_options)
    # 屏幕方向（Tab5 专用；其它 target 定义了也不引用）：0 = 竖屏（默认）1 = 横屏。
    # 见 components/retro-go/targets/tab5/config.h 的 RG_TAB5_ORIENTATION。
    # ⚠ 必须**每次显式传**（rg_tool.py 保证）—— CMake 缓存变量是"粘"的，漏传会沿用上一次的值，
    #   症状：本该竖屏的构建实际编成了横屏。
    if(NOT DEFINED RG_TAB5_ORIENTATION)
        set(RG_TAB5_ORIENTATION 0)
    endif()

    # PPA 传输模式实验开关（Tab5 横屏专用）：0 = 关（默认）1 = 非阻塞 2 = BLOCKING 对照。
    # 同样必须每次显式传（理由同上）。⚠ 另一处副本在 components/retro-go/CMakeLists.txt。
    if(NOT DEFINED RG_TAB5_PPA_MODE)
        set(RG_TAB5_PPA_MODE 0)
    endif()

    component_compile_options(
        -D${RG_BUILD_TARGET}=1
        -DRG_TAB5_ORIENTATION=${RG_TAB5_ORIENTATION}
        -DRG_TAB5_PPA_MODE=${RG_TAB5_PPA_MODE}
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
        # ⚠ 这里只放不带引号的 RG_SINGLE_APP：宏里的转义引号会被二次转义（写成 \"gbsp\" 最终
        #   变成 \\\\gbsp），所以 RG_SINGLE_APP_CORE 放在 components/retro-go/CMakeLists.txt
        #   里（那边不走宏，转义正常）。
        # ⚠ 只用 rg_setup_compile_options() 的组件才会吃到这里 —— retro-go 组件自己写了一套
        #   component_compile_options()，它也单独加了一份，改的时候两处都要动。
        component_compile_options(-DRG_SINGLE_APP=1)
    endif()
endmacro()
