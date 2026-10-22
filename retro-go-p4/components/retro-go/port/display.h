#pragma once
/* ============================================================================
 * 板级显示驱动**契约**（port 层）—— 把"只写在注释里的契约"变成编译期可检查
 * ----------------------------------------------------------------------------
 * 为什么不是函数指针表（vtable）：显示驱动都在 drivers/display/ 下，由 rg_display.c
 * 用 `#include` 拉进**同一个 TU**，函数是 `static` / `static inline`，热路径（每帧几十次
 * block 推送）靠内联。改成 vtable = 间接调用 + 丢内联 ⇒ 显示通路性能回退。
 * 所以这里的做法是：**同一套原型声明**，谁实现谁就把它们定义出来 —— 少一个函数、
 * 或参数类型不一致，**编译期就报错**（不再是靠人读注释/runtime 才知道对不上）。
 *
 * 谁要实现（三份，几何/后端各不同，接口相同）：
 *   drivers/display/mipi_dsi_tab5.h      Tab5 横屏后端（被 tab5_l_api.c 包一层）
 *   drivers/display/mipi_dsi_tab5_p.h    Tab5 竖屏后端（被 tab5_p_api.c 包一层）
 *   drivers/display/tab5_dispatch.h      Tab5 单 app：运行时选上面两份（转发器）
 *
 * ⚠ 本头文件**只能被"确实定义了这 8 个函数"的 TU 包含**（现在是上面三份各自包含）——
 *   它声明的是 `static` 函数，在没有定义的 TU 里包含会得到"声明了但未定义"的编译错误。
 *   这正是契约的一部分：想让别的目标也纳入契约，就先把那 8 个函数补齐。
 *
 * 上游遗留驱动（ili9341.h / sdl2.h / dummy.h）暂未纳入本契约（它们的符号集不同），
 * 归属见台账 P4；rg_display.c 的 `#if` 分发仍兼容它们的老编号。
 * ==========================================================================*/

#include <stddef.h>
#include <stdint.h>

/* ── 驱动代号：把 rg_display.c / 各 driver 里的"魔法数字"变成具名常量 ──────────
 * 取值沿用上游 `RG_SCREEN_DRIVER` 的编号（不改数值 ⇒ 行为零变化），
 * 新代码一律用名字，不再直接写 2/3/4/5。 */
#define RG_DISPLAY_DRIVER_ILI9341    0    /* SPI 小屏（上游，ILI9341/ST7789） */
#define RG_DISPLAY_DRIVER_TAB5_L     2    /* Tab5 横屏后端（90° 映射） */
#define RG_DISPLAY_DRIVER_TAB5_P     3    /* Tab5 竖屏后端（线性） */
#define RG_DISPLAY_DRIVER_TAB5_BOTH  4    /* Tab5 单 app：两份都编，运行时由分发层选 */
#define RG_DISPLAY_DRIVER_SDL2       99   /* 宿主（macOS/Linux 预览） */
#define RG_DISPLAY_DRIVER_DUMMY      100  /* 无显示（"以上都不是"） */

/* ── 契约宏：由**驱动自己**在 8 个函数都定义之后调用一次 ─────────────────────
 * 为什么必须放在"定义之后"（这是本契约的牙齿所在）：宏里用 `__typeof__(lcd_init)` 这类写法
 * 逐个引用这些名字，而**本头文件刻意不预先声明它们** ⇒ 驱动一旦把它们改名/漏实现，
 * 展开点就找不到标识符 ⇒ **编译期直接红**。
 * 代价为零：纯静态断言，不生成任何代码（已用"剥 DWARF 后 .obj 逐字节相同"验证过）。
 *
 * 用法（三份驱动都在文件末尾调用）：`RG_DISPLAY_CONTRACT();`
 * ⚠ 漏调用宏就失去检查 —— tools/check-display-contract.py 会 grep 这三份文件必须含该调用。 */
#define RG_DISPLAY_CONTRACT()                                                                     \
    _Static_assert(__builtin_types_compatible_p(__typeof__(lcd_init), void (void)),               \
                   "显示驱动契约：缺 lcd_init 或签名应为 void(void)");                            \
    _Static_assert(__builtin_types_compatible_p(__typeof__(lcd_deinit), void (void)),             \
                   "显示驱动契约：缺 lcd_deinit 或签名应为 void(void)");                          \
    _Static_assert(__builtin_types_compatible_p(__typeof__(lcd_sync), void (void)),               \
                   "显示驱动契约：缺 lcd_sync 或签名应为 void(void)");                            \
    _Static_assert(__builtin_types_compatible_p(__typeof__(lcd_set_rotation), void (int)),        \
                   "显示驱动契约：缺 lcd_set_rotation 或签名应为 void(int)");                     \
    _Static_assert(__builtin_types_compatible_p(__typeof__(lcd_set_backlight), void (float)),     \
                   "显示驱动契约：缺 lcd_set_backlight 或签名应为 void(float)");                  \
    _Static_assert(__builtin_types_compatible_p(__typeof__(lcd_set_window), void (int, int, int, int)), \
                   "显示驱动契约：缺 lcd_set_window 或签名应为 void(int,int,int,int)");           \
    _Static_assert(__builtin_types_compatible_p(__typeof__(lcd_get_buffer), uint16_t *(size_t)),  \
                   "显示驱动契约：缺 lcd_get_buffer 或签名应为 uint16_t *(size_t)");              \
    _Static_assert(__builtin_types_compatible_p(__typeof__(lcd_send_buffer), void (uint16_t *, size_t)), \
                   "显示驱动契约：缺 lcd_send_buffer 或签名应为 void(uint16_t*,size_t)")
