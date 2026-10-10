/* 0.4.9 单 app：横屏显示后端的包装 TU（见 tab5_l_api.h / tab5_p_api.h 的说明）。
 * ⚠ 本文件会被组件级 glob 编进**所有目标**，所以整段包在驱动号判断里：
 *   非 Tab5 目标编译成空 TU（否则会找不到 mipi_dsi_tab5.h / esp_lcd 头）。 */
#include "rg_system.h"              /* 目标配置（RG_SCREEN_* / RG_TAB5_PPA_MODE / RG_SCREEN_INIT） */

#if RG_SCREEN_DRIVER == 2 || RG_SCREEN_DRIVER == 3 || RG_SCREEN_DRIVER == 4
/* 4 = Tab5 单 app（两份驱动都编，运行时由分发层选）—— 见 targets/tab5/config.h 的说明 */
#include "drivers/display/tab5_l_api.h"
#include "drivers/display/mipi_dsi_tab5.h"   /* 横屏驱动本体（static，本 TU 私有） */

void tab5l_init(void)                          { lcd_init(); }
void tab5l_deinit(void)                        { lcd_deinit(); }
void tab5l_sync(void)                          { lcd_sync(); }
void tab5l_set_rotation(int rotation)          { lcd_set_rotation(rotation); }
void tab5l_set_backlight(float percent)        { lcd_set_backlight(percent); }
void tab5l_set_window(int l, int t, int w, int h) { lcd_set_window(l, t, w, h); }
uint16_t *tab5l_get_buffer(size_t length)      { return lcd_get_buffer(length); }
void tab5l_send_buffer(uint16_t *b, size_t n)  { lcd_send_buffer(b, n); }

#endif  /* RG_SCREEN_DRIVER == 2 || 3 */
