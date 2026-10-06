/* ============================================================================
 * 触摸皮肤（控制区面板 + 按键配色）—— 公开 API
 * ----------------------------------------------------------------------------
 * 四套皮肤都能选（菜单里切），每套 = ①控制区面板（分区色块/细线/凹槽/机型铭牌）
 *                                    ②每键配色 + 填充/标签配方
 * 全部数据与绘制在 rg_touch_skin.c（**唯一真源**：PC 预览工具 tools/preview-skin.py
 * 解析同一个文件，所以"PC 稿"与"固件将要画的"不可能漂移）。
 *
 * 为什么面板不塞图片进 flash：分区/细线/凹槽/铭牌都是程序化画的（几张色块 + 一个
 * 8x8 字库的机型名），生成一张 720x1280 的 PSRAM surface 当"边框图"用 —— 走的是
 * rg_display.c 里现成的 border 通路（开机铺一次 + 脏条带重建时取用），零新增机制。
 * 代价：1.84MB PSRAM（与用户自己选了 Border 图时完全一样）。
 * ==========================================================================*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rg_system.h"      /* rg_key_t + 屏幕尺寸（config.h） */
#include "rg_surface.h"

/* 与 rg_touch_overlay 同一个开关：宿主/无触摸 target 上整个模块编成空，
 * 免得"调用留着、声明被编掉"（2026-10-03 SDL2 宿主构建就是这么坏的）。 */
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY

#define RG_TOUCH_SKIN_COUNT 4
#define RG_TOUCH_SKIN_DEFAULT 3   /* 默认皮肤：D 套「主机配色点缀」（用户选定） */

/* 皮肤槽位：与键位解耦的固定序号（皮肤表按这个序存颜色）。
 * 单独定义的原因：皮肤要覆盖"某机型可能没有的键"（GB 没有 L/R、NES 没有 X/Y），
 * 所以配色表必须按**功能**排，而不是按某个机型的键位表排。 */
typedef enum
{
    RG_SKIN_SLOT_UP = 0, RG_SKIN_SLOT_DOWN, RG_SKIN_SLOT_LEFT, RG_SKIN_SLOT_RIGHT,
    RG_SKIN_SLOT_A, RG_SKIN_SLOT_B, RG_SKIN_SLOT_X, RG_SKIN_SLOT_Y,
    RG_SKIN_SLOT_L, RG_SKIN_SLOT_R, RG_SKIN_SLOT_SELECT, RG_SKIN_SLOT_START,
    RG_SKIN_SLOT_MENU, RG_SKIN_SLOT_SWAP,
    RG_SKIN_SLOT_COUNT
} rg_skin_slot_t;

int  rg_touch_skin_count(void);
int  rg_touch_skin_clamp(int idx);
const char *rg_touch_skin_name(int idx);         /* ASCII 全名（选择器列表用；设备字库只有 8x8 ASCII） */
const char *rg_touch_skin_short_name(int idx);   /* ASCII 短名（设置项右列用） */
int  rg_touch_skin_slot_for_key(rg_key_t key);   /* rg_key_t → 槽位（RG_KEY_NONE → SWAP 槽，见 .c 注释） */
uint16_t rg_touch_skin_key_color(int skin_idx, int slot, const char *console_id);
/* 按键三层配方（边框/填充/标签的**覆盖率权重**，0~255）：
 * B 套是线框皮肤（填充层权重 0），所以不能写死成编译期常量。 */
void rg_touch_skin_tiers(int skin_idx, uint8_t out_tiers[3]);   /* 序：border, fill, label */
void rg_touch_skin_button_recipe(int skin_idx, int *radius_pct, int *fill_pct, int *label_pct,
                                 bool *outline_only);
/* 标签颜色覆盖（C 套统一 #FFD98A）：返回 false 表示"用 tint(键色, label%)" */
bool rg_touch_skin_label_color(int skin_idx, uint16_t *out);

/* 机型铭牌文案（按核心的 configNs：gba/gb/gbc/nes/...）+ 该机型的点缀色。
 * 找不到时退回通用值（"RETRO-GO" + 琥珀），不返回 NULL。 */
const char *rg_touch_skin_badge(const char *console_id);
uint16_t    rg_touch_skin_accent(int skin_idx, const char *console_id);

/* 面板底图：720x1280，**原地复用**（第一次分配，之后每次皮肤/机型变化原地重画）。
 * 返回的 surface 所有权属于本模块（不归 rg_display），可以直接交给
 * rg_display_set_border_surface() 当边框用。
 *   ctrl_top    = 游戏视口底边（GBA 480；多核后按各机型窗口高度传）
 *   win_x/win_w = 游戏视口横向范围（屏幕凹槽围着它画 —— 整数缩放的边条靠它读成 bezel）
 *   led_cy/led_r = 电量圆灯的位置/R，用于把铭牌放到"菱形键底 ~ 圆灯顶"的正中 */
rg_surface_t *rg_touch_panel_get(int skin_idx, const char *console_id, int ctrl_top,
                                 int win_x, int win_w, int led_cy, int led_r);

#endif /* defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY */
