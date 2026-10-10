#pragma once
/* 0.4.9 单 app：Tab5 双方向显示后端的分发层。
 *
 * 契约：**rg_display.c 的调用点一字不改** —— 这里把通用显示 API（lcd_*）定义成 static inline
 * 分发器，按运行时方向（rg_orient_active()）转给横屏(tab5l_*)或竖屏(tab5p_*)后端。
 *
 * 为什么是运行时分发：0.4.9 要"一个 app 同时装两个方向"（用户 2026-10-10 定），
 * 而两份驱动（mipi_dsi_tab5.h / mipi_dsi_tab5_p.h）定义了完全相同的一套 static 符号。
 * 共存办法 = 各自包进自己的 TU（tab5_l_api.c / tab5_p_api.c）：驱动内部一行不改，
 * 符号天然隔离；本文件只做"按方向选一份"。
 *
 * 双 app 形态（RG_SINGLE_APP=0）下 rg_orient_active() 返回**运行分区标签**，
 * 于是分发结果与改造前逐字一致 —— 这条路径必须零回归（真机对照 r3/r4 基线验证）。
 */
#include <stddef.h>
#include <stdint.h>
#include "rg_orient.h"
#include "drivers/display/tab5_l_api.h"
#include "drivers/display/tab5_p_api.h"

/* 粘性后端选择：**本进程生命周期内只判一次**，在 lcd_init() 里锁定。
 *
 * 为什么必须粘住（否则会黑屏/崩）：改方向的路径是"写 NVS（或设置）→ 重启"。在重启真正发生前的
 * 那几百毫秒里，NVS 已经是**新**方向，而**另一份后端还没 init**（帧缓冲未分配、DSI 未起）。
 * 若每次调用都现查 rg_orient_active()，这期间所有显示调用都会被转到那个未初始化的后端。
 * 锁定后语义变干净：**写 NVS 只影响下次启动**，本次运行到重启为止始终用同一份后端。
 * 双 app 形态同样受益（运行分区标签本来就不变 ⇒ 锁定 = 零行为差异）。 */
static int tab5_dispatch_backend __attribute__((unused)) = -1;  /* -1 未锁定 / 0 竖屏 / 1 横屏 */

static inline bool tab5_dispatch_landscape(void)
{
    if (tab5_dispatch_backend < 0)   /* 未锁定（只应在 lcd_init 之前被问到）⇒ 临时按真值判，不落锁 */
        return rg_orient_active() == RG_ORIENT_LANDSCAPE;
    return tab5_dispatch_backend == 1;
}

static inline void tab5_dispatch_settle(void)
{
    tab5_dispatch_backend = (rg_orient_active() == RG_ORIENT_LANDSCAPE) ? 1 : 0;
}

static inline void lcd_init(void)
{
    tab5_dispatch_settle();          /* 锁定本次运行的后端：之后所有 lcd_* 都走它 */
    if (tab5_dispatch_landscape()) tab5l_init(); else tab5p_init();
}
static inline void lcd_deinit(void)
{
    if (tab5_dispatch_landscape()) tab5l_deinit(); else tab5p_deinit();
}
static inline void lcd_sync(void)
{
    if (tab5_dispatch_landscape()) tab5l_sync(); else tab5p_sync();
}
static inline void lcd_set_rotation(int rotation)
{
    if (tab5_dispatch_landscape()) tab5l_set_rotation(rotation); else tab5p_set_rotation(rotation);
}
static inline void lcd_set_backlight(float percent)
{
    if (tab5_dispatch_landscape()) tab5l_set_backlight(percent); else tab5p_set_backlight(percent);
}
static inline void lcd_set_window(int left, int top, int width, int height)
{
    if (tab5_dispatch_landscape()) tab5l_set_window(left, top, width, height);
    else tab5p_set_window(left, top, width, height);
}
static inline uint16_t *lcd_get_buffer(size_t length)
{
    return tab5_dispatch_landscape() ? tab5l_get_buffer(length) : tab5p_get_buffer(length);
}
static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (tab5_dispatch_landscape()) tab5l_send_buffer(buffer, length);
    else tab5p_send_buffer(buffer, length);
}
