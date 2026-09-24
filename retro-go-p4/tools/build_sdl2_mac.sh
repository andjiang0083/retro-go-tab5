#!/bin/bash
# macOS/SDL2 宿主构建（开发用，非发布）
#
# 用途：在 Mac 上跑 retro-go 的 launcher / gbsp，省去真机往返，
#       快速验证【应用接线】与【模拟器逻辑】。
#
# 限制（务必知道，别指望它替代真机）：
#   宿主没有 P4 的 MMU 可执行映射 / DSI / PPA / PSRAM，也没有 RISC-V dynarec
#   → 无法复现 P4 专属问题（JIT 可执行映射、显示路径、带宽仲裁）。
#
# 参考：tools/build_sdl2.sh（旧版布局，指向已废弃的 retro-core/）
#
# 用法：sh tools/build_sdl2_mac.sh            # 全部（launcher + gbsp）
#       sh tools/build_sdl2_mac.sh overlay    # 只构建"虚拟按键可视层预览"（tab5 分辨率+键位表）
set -e
cd "$(dirname "$0")/.."

# 注意：PATH 里可能有 ESP-IDF 的 esp-clang（RISC-V 交叉编译器），必须显式用系统 clang
CC="${CC:-/usr/bin/clang}"
[ -x "$CC" ] || CC="$(xcrun --find clang 2>/dev/null || echo clang)"
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
SDL_CFLAGS="-I$BREW_PREFIX/include $(sdl2-config --cflags)"
SDL_LIBS="$(sdl2-config --libs)"

# _DARWIN_C_SOURCE: miniz.c 用 utime()，macOS 的 C99 默认不暴露
COMMON_CFLAGS="-DRG_TARGET_SDL2 -DRETRO_GO -DCJSON_HIDE_SYMBOLS -DSDL_MAIN_HANDLED=1 \
-DRG_BUILD_INFO=\"SDL2-mac\" -Dapp_main=SDL_Main -D_DARWIN_C_SOURCE \
-include utime.h \
$SDL_CFLAGS -Itools/sdl2-compat -Icomponents/retro-go -Icomponents/retro-go/libs/cJSON \
-Icomponents/retro-go/libs/lodepng -Icomponents/retro-go/libs/miniz"

RG_SRCS="components/retro-go/*.c components/retro-go/drivers/audio/*.c \
components/retro-go/fonts/*.c components/retro-go/libs/cJSON/*.c \
components/retro-go/libs/lodepng/*.c components/retro-go/libs/miniz/*.c"

LIBS="$SDL_LIBS -lc++"

MODE="${1:-all}"

mkdir -p build-sdl2
rm -f build-sdl2/launcher build-sdl2/gbsp build-sdl2/overlay-preview

if [ "$MODE" = "all" ] || [ "$MODE" = "launcher" ]; then
echo "=== [1/2] 构建 launcher（完整输出，不吞错误）==="
$CC $COMMON_CFLAGS $RG_SRCS launcher/main/*.c $LIBS -o build-sdl2/launcher

echo "=== launcher 产物 ==="
ls -l build-sdl2/launcher
fi

# ── gbsp（GBA 模拟器）──────────────────────────────────────────────────────────
# 宿主上不定义 HAVE_DYNAREC：gpSP 的 dynarec 只有 x86/ARM32/MIPS 后端，
# 本机是 arm64 → 走解释器（cpu_threaded.c），速度足够开发验证。
# 排除 gpsp_memory_alloc.c（ESP 专属：EXT_RAM_BSS_ATTR / PSRAM）；
# 不定义 HAVE_DYNAREC 时，全局量由 cpu.cpp 的 #ifndef HAVE_DYNAREC 段定义。
GBSP_DIR="gbsp/components/gbsp-libretro"
# 显式列表：排除 gpsp_memory_alloc.c（ESP 专属）与 libretro/（libretro 外壳）
GBSP_SRCS="$GBSP_DIR/cheats.c $GBSP_DIR/cpu_threaded.c $GBSP_DIR/gba_cc_lut.c \
$GBSP_DIR/gba_memory.c $GBSP_DIR/gbp.c $GBSP_DIR/input.c $GBSP_DIR/main.c \
$GBSP_DIR/memmap.c $GBSP_DIR/rfu.c $GBSP_DIR/savestate.c $GBSP_DIR/serial.c \
$GBSP_DIR/sound.c $GBSP_DIR/cpu.cpp $GBSP_DIR/video.cpp"

if [ "$MODE" = "all" ] || [ "$MODE" = "gbsp" ]; then
echo "=== [2/2] 构建 gbsp ==="
$CC $COMMON_CFLAGS \
  -I$GBSP_DIR -I$GBSP_DIR/libretro/libretro-common/include -Igbsp/main \
  $RG_SRCS gbsp/main/*.c \
  $GBSP_SRCS \
  $LIBS -o build-sdl2/gbsp

echo "=== gbsp 产物 ==="
ls -l build-sdl2/gbsp
fi

# ── 虚拟按键可视层预览（tab5 分辨率 + tab5 键位表，跑同一套渲染代码）──────────
# 为什么值得单独一个二进制：可视层的标签/透明度/按下反馈改一次要刷一次机才知道好不好看，
# 这里能在 Mac 上直接出图（配合 RG_SDL2_SHOT 截图钩子），是"PC 重渲染 + 视觉评审"的一环。
if [ "$MODE" = "overlay" ]; then
echo "=== 构建 overlay-preview（tab5 可视层预览）==="
$CC $COMMON_CFLAGS -DRG_TAB5_OVERLAY_PREVIEW \
  $RG_SRCS launcher/main/*.c \
  $LIBS -o build-sdl2/overlay-preview
echo "=== overlay-preview 产物 ==="
ls -l build-sdl2/overlay-preview
fi
