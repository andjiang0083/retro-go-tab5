#pragma once
/* 0.4.9 单 app：Tab5 显示后端 —— **竖屏**（原 drivers/display/mipi_dsi_tab5_p.h）的对外句柄。
 *
 * 为什么要有这一层：横竖两份驱动定义了**完全相同的一套符号**（lcd_init / lcd_sync / tab5_draw …）
 * 且都是 static。要做"一个 app 里两个方向"，就必须让它们能同时存在于同一个二进制：
 * 把各自的驱动头文件**编进一个独立 TU**（.c），驱动的 34 个文件级 static 天然被隔离，
 * 驱动内部**一行都不用改**，对外只暴露本文件这套带前缀的薄接口。
 *
 * 术语：竖屏后端 = tab5p_*（p = portrait），横屏后端 = tab5l_*（l = landscape）。
 * 选择在 drivers/display/tab5_dispatch.h 里按运行时方向做（rg_orient_active()）。 */
#include <stddef.h>
#include <stdint.h>

void      tab5p_init(void);
void      tab5p_deinit(void);
void      tab5p_sync(void);
void      tab5p_set_rotation(int rotation);
void      tab5p_set_backlight(float percent);
void      tab5p_set_window(int left, int top, int width, int height);
uint16_t *tab5p_get_buffer(size_t length);
void      tab5p_send_buffer(uint16_t *buffer, size_t length);
