/* 0.4.9 单 app：竖屏显示后端的包装 TU（见 tab5_p_api.h 的说明）。
 * ⚠ 本文件会被组件级 glob 编进**所有目标**，所以整段包在驱动号判断里：
 *   非 Tab5 目标编译成空 TU（否则会找不到 mipi_dsi_tab5_p.h / esp_lcd 头）。 */
#include "rg_system.h"              /* 目标配置（RG_SCREEN_* / RG_TAB5_PPA_MODE / RG_SCREEN_INIT） */

#if RG_SCREEN_DRIVER == 2 || RG_SCREEN_DRIVER == 3 || RG_SCREEN_DRIVER == 4
/* 4 = Tab5 单 app（两份驱动都编，运行时由分发层选）—— 见 targets/tab5/config.h 的说明 */
#include "drivers/display/tab5_p_api.h"
/* ⚠ 跨 TU 依赖：竖屏驱动会调用 rg_display.c 的上报函数（过去靠"驱动被 #include 进 rg_display.c
 * 同一 TU"而隐式可见）。驱动现在编在独立 TU 里，所以这里补一条声明 —— 驱动本体一字不改。
 * 只声明、不定义：真正的实现在 rg_display.c（含推送失败计数与屏幕角标）。 */
void rg_display_push_failed(int top, int lines);
#include "drivers/display/mipi_dsi_tab5_p.h"  /* 竖屏驱动本体（static，本 TU 私有） */

void tab5p_init(void)                          { lcd_init(); }
void tab5p_deinit(void)                        { lcd_deinit(); }
void tab5p_sync(void)                          { lcd_sync(); }
void tab5p_set_rotation(int rotation)          { lcd_set_rotation(rotation); }
void tab5p_set_backlight(float percent)        { lcd_set_backlight(percent); }
void tab5p_set_window(int l, int t, int w, int h) { lcd_set_window(l, t, w, h); }
uint16_t *tab5p_get_buffer(size_t length)      { return lcd_get_buffer(length); }
void tab5p_send_buffer(uint16_t *b, size_t n)  { lcd_send_buffer(b, n); }

#endif  /* RG_SCREEN_DRIVER == 2 || 3 */
