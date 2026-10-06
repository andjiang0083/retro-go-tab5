/* 宿主也编诊断探针（与真机同一套 DIAG_*）。⚠ 必须放顶层：
 * 之前塞进 #ifdef RG_TAB5_OVERLAY_PREVIEW 里，普通宿主构建根本看不到。 */
#ifndef RG_GBA_DIAG
#define RG_GBA_DIAG 1
#endif

// Target definition
#define RG_TARGET_NAME             "SDL2"

// Storage
#define RG_STORAGE_ROOT             "./sd"  // Storage mount point

// Audio
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        0   // 0 = Disable, 1 = Enable
#define RG_AUDIO_USE_SDL2           1   // 0 = Disable, 1 = Enable

// Video
#define RG_SCREEN_DRIVER            99   // 0 = ILI9341/ST7789
#define RG_SCREEN_HOST              0
#define RG_SCREEN_SPEED             0
#define RG_SCREEN_BACKLIGHT         1
/* ── Tab5 虚拟按键可视层的 PC 预览模式（宿主构建专用）─────────────────────────
 * 用 tab5 的真实分辨率 + 真实键位表在 Mac 上渲染可视层，验证标签/透明度/按下反馈，
 * 省掉"改一次刷一次机"。构建方式见 tools/build_sdl2_mac.sh 的 -DRG_TAB5_OVERLAY_PREVIEW。
 * 只影响宿主构建，不影响任何真机 target。 */
#ifdef RG_TAB5_OVERLAY_PREVIEW
#define RG_SCREEN_WIDTH             1280
#define RG_SCREEN_HEIGHT            720
#include "../tab5/touch_layout.h"
#define RG_GAMEPAD_TOUCH_MAP        RG_TAB5_TOUCH_MAP
#else
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#endif

/* ── Tab5 显示几何仿真（宿主专用，2026-10-06）─────────────────────────────────
 * 用途：让**共享的** rg_display.c（缩放映射 / 逐行校验和 / 32 行块推送）在宿主上走
 *       与 Tab5 真机同一条路径，然后用 SDL2 驱动的 canvas（它按"窗口 + 写指针推进"
 *       写像素，语义等同面板帧缓冲）验证"小面积变化有没有真的到达面板"。
 * 为什么需要它：真机症状是"静态菜单里方向键不生效、游戏内正常"—— 差别正是
 *       只推变化行 vs 整屏都在变。宿主默认 320x240/1:1 走不到那条分支（缩放=1、
 *       块边界也不同），所以必须把几何和块大小照抄过来。
 * 打开方式：构建时加 -DRG_TAB5_DISPLAY_EMU（见 tools/build_sdl2_mac.sh）。 */
#ifdef RG_TAB5_DISPLAY_EMU
#undef RG_SCREEN_WIDTH
#undef RG_SCREEN_HEIGHT
#define RG_SCREEN_WIDTH             720
#define RG_SCREEN_HEIGHT            1280
#undef RG_SCREEN_VISIBLE_AREA
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 800}   /* 同 tab5/config.h */
#define RG_SCREEN_PARTIAL_UPDATES   1
#define LCD_BUFFER_LENGTH           (RG_SCREEN_WIDTH * 32)   /* 同 tab5：32 行一块 */
#define RG_DISPLAY_DEFAULT_SCALING     RG_DISPLAY_SCALING_ZOOM
#define RG_DISPLAY_DEFAULT_CUSTOM_ZOOM 3.0
#define RG_DISPLAY_MAX_CUSTOM_ZOOM     4.0
#endif
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
#define RG_SCREEN_INIT()

// Input
// Refer to rg_input.h to see all available RG_KEY_* and RG_GAMEPAD_*_MAP types
#define RG_GAMEPAD_KBD_MAP {\
    {RG_KEY_UP,     SDL_SCANCODE_UP},\
    {RG_KEY_RIGHT,  SDL_SCANCODE_RIGHT},\
    {RG_KEY_DOWN,   SDL_SCANCODE_DOWN},\
    {RG_KEY_LEFT,   SDL_SCANCODE_LEFT},\
    {RG_KEY_SELECT, SDL_SCANCODE_0},\
    {RG_KEY_START,  SDL_SCANCODE_SPACE},\
    {RG_KEY_MENU,   SDL_SCANCODE_ESCAPE},\
    {RG_KEY_OPTION, SDL_SCANCODE_TAB},\
    {RG_KEY_A,      SDL_SCANCODE_X},\
    {RG_KEY_B,      SDL_SCANCODE_Z},\
    {RG_KEY_X,      SDL_SCANCODE_S},\
    {RG_KEY_Y,      SDL_SCANCODE_A},\
    {RG_KEY_L,      SDL_SCANCODE_Q},\
    {RG_KEY_R,      SDL_SCANCODE_W},\
}

// Battery
// #define RG_BATTERY_ADC_CHANNEL      ADC1_CHANNEL_0
// #define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - 3500.f) / (4200.f - 3500.f) * 100.f)
// #define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)

#if !defined(__VERSION__) && defined(__TINYC__)
#define __VERSION__ "TinyC"
#endif

#undef app_main
#define app_main(...) main(int argc, char **argv)
// #define rg_system_init(a, b, c) rg_system_init(argc, argv, a, b, c)
