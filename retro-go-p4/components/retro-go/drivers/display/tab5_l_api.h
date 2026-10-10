#pragma once
/* 0.4.9 单 app：Tab5 显示后端 —— **横屏**（原 drivers/display/mipi_dsi_tab5.h）的对外句柄。
 * 说明与设计理由见 tab5_p_api.h（两者对称：l = landscape，p = portrait）。 */
#include <stddef.h>
#include <stdint.h>

void      tab5l_init(void);
void      tab5l_deinit(void);
void      tab5l_sync(void);
void      tab5l_set_rotation(int rotation);
void      tab5l_set_backlight(float percent);
void      tab5l_set_window(int left, int top, int width, int height);
uint16_t *tab5l_get_buffer(size_t length);
void      tab5l_send_buffer(uint16_t *buffer, size_t length);
