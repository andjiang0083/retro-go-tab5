set(COMPONENT_SRCDIRS ". drivers/audio fonts libs/netplay libs/lodepng")
set(COMPONENT_ADD_INCLUDEDIRS ". libs/netplay libs/lodepng")
if((IDF_VERSION_MAJOR EQUAL 5) AND (IDF_VERSION_MINOR GREATER 2)) # esp-idf 5.3+
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client esp_driver_ledc esp_driver_i2c esp_driver_ppa esp_mm driver esp_lcd m5stack_tab5 esp_codec_dev ppa_engine")
    if(RG_BUILD_TARGET STREQUAL "RG_TARGET_TAB5")
        # Tab5 的触摸虚拟手柄要用 ST7123 触摸组件（BSP 已依赖它，这里显式声明以便本组件 include 其头文件）
        list(APPEND COMPONENT_REQUIRES esp_lcd_touch esp_lcd_touch_st7123)
    endif()
elseif(IDF_VERSION_MAJOR EQUAL 5) # esp-idf 5.0 - 5.2
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update json nvs_flash esp_adc esp_timer esp_psram esp_wifi esp_http_client")
else() # esp-idf 4.1 - 4.4
    set(COMPONENT_REQUIRES "spi_flash fatfs app_update esp_adc_cal json nvs_flash esp_wifi esp_http_client")
endif()

register_component()

# 内置 CJK 点阵字库：**编进 app 镜像**（rg_cjk.c 里用 _binary_cjk12_bin_start/_end 直接读）。
# 为什么不放 flash 分区：M5Launcher 这类"只装一个 app 镜像"的启动器会把非 sys/system 的
# 数据分区按它自己的规则重建，按名字找分区的做法在那条安装路径上必然拿不到字体。
# 代价只是每个 app 各带一份 103KB（Tab5 只有 launcher + gbsp 两个 app，分区表自动扩容）。
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../../assets/cjk12.bin" BINARY)

# Small size is preferred because of the small cache and most things here aren't performance sensitive!
# rg_display and rg_audio benefit from higher optimization (which of -O2 or -O3 is better depends...)

component_compile_options(
    -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    -DLODEPNG_NO_COMPILE_ERROR_TEXT
    -Os
    -Wno-unused-function
)
set_source_files_properties(
    rg_audio.c rg_display.c rg_utils.c rg_surface.c rg_touch_skin.c libs/lodepng/lodepng.c
    PROPERTIES COMPILE_FLAGS
    -O2
)

if(RG_ENABLE_NETPLAY)
    component_compile_options(-DRG_ENABLE_NETWORKING -DRG_ENABLE_NETPLAY)
elseif(RG_ENABLE_NETWORKING)
    component_compile_options(-DRG_ENABLE_NETWORKING)
endif()

if(RG_ENABLE_PROFILING)
    component_compile_options(-DRG_ENABLE_PROFILING)
endif()

if(RG_PROJECT_VER)
    component_compile_options(-DRG_PROJECT_VER="${RG_PROJECT_VER}")
endif()

if(RG_BUILD_RELEASE)
    component_compile_options(-DRG_BUILD_RELEASE=1)
endif()

set(RG_BUILD_INFO "${RG_BUILD_TARGET}-${RG_BUILD_RELEASE} ${PROJECT_VER} SDK:${IDF_VER} ${IDF_TARGET}")
string(TIMESTAMP RG_BUILD_TIME "%s")

component_compile_options(-D${RG_BUILD_TARGET}=1)
# 屏幕方向（Tab5 专用）：0 = 竖屏（默认）1 = 横屏。⚠ 本组件不走 rg_setup_compile_options()
# （见下面那条注释），所以这里必须单独再加一份 —— 改的时候两处都要动。
if(NOT DEFINED RG_TAB5_ORIENTATION)
    set(RG_TAB5_ORIENTATION 0)
endif()
component_compile_options(-DRG_TAB5_ORIENTATION=${RG_TAB5_ORIENTATION})
# PPA 传输模式实验开关（Tab5 横屏专用）：0 = 关（默认）1 = 非阻塞 2 = BLOCKING 对照。
# ⚠ 同上一行：本组件不走 rg_setup_compile_options()，两处副本都要加（另一处在 base.cmake）。
if(NOT DEFINED RG_TAB5_PPA_MODE)
    set(RG_TAB5_PPA_MODE 0)
endif()
component_compile_options(-DRG_TAB5_PPA_MODE=${RG_TAB5_PPA_MODE})
# ⚠ 本组件不走 rg_setup_compile_options()（那是个宏，只对调用它的组件生效），单 app 形态的
# 宏必须在下面这组里单独加 —— 否则 rg_system.c 编进去的还是老版本，表现为菜单里游戏列表全空。
if(RG_SINGLE_APP)
    # 只传不带引号的开关：带引号的宏在 CMake 里要过两层转义（实测被转义坏），
    # 核心名用 C 侧默认值 + 字符串化，见 rg_system.c 的 RG_SINGLE_APP_CORE。
    component_compile_options(-DRG_SINGLE_APP=1)
endif()
# Tab5 BSP 的 umbrella 头（bsp/m5stack_tab5.h）默认会 #include "lvgl.h"，而 retro-go 不编 LVGL。
# 该开关在 BSP 里是 PRIVATE 定义（不会传给使用方），所以使用方必须自己加一份。
component_compile_options(-DBSP_CONFIG_NO_GRAPHIC_LIB=1)
component_compile_options(-DRG_PROJECT_APP="${RG_PROJECT_APP}")
component_compile_options(-DRG_BUILD_INFO="${RG_BUILD_INFO}")
component_compile_options(-DRG_BUILD_TIME=${RG_BUILD_TIME})

message(STATUS "RG_PROJECT_APP: ${RG_PROJECT_APP}")
message(STATUS "RG_PROJECT_VER: ${RG_PROJECT_VER}")
message(STATUS "RG_BUILD_INFO: ${RG_BUILD_INFO}")
