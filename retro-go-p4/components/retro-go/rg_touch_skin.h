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
#include <string.h>       /* 机型判据用 strcmp（头文件里的 static inline 也要能独立编译） */

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
/* 覆盖层/显示驱动要跟面板用**同一套**几何（各写一份常量必然漂移）：圆灯中心 y。
 * ctrl_top 传控制区顶（覆盖层是 RG_OVERLAY_CTRL_TOP），led_r 传灯半径。 */
int rg_touch_skin_led_cy(int ctrl_top, int led_r);
/* 面板底色（565）：驱动擦"电量灯条带"时用它当背景 —— 用纯黑会在非黑面板上割裂画面。 */
uint16_t rg_touch_skin_panel_bg565(int skin_idx);
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
/* launcher（菜单）不是游戏机型：它要能操作所有键（菜单用方向键 + A/B 选择，X/Y/L/R 也可能
 * 被当作快捷键）→ 一律给"GBA 那套"全键。
 * ⚠ console_id 传进来的是 app->configNs，**菜单自己就是 "launcher"、不是 NULL/空串**
 *   （rg_touch_overlay.c 从 app->configNs 拷来）—— 2026-10-07 的坑：只判了空字符串，
 *   菜单于是落进"非 GBA"分支，L/R 和 X/Y 被静默隐藏，菜单少了键。 */
static inline bool rg_touch_is_menu(const char *console_id)
{
    return !console_id || !*console_id || strcmp(console_id, "launcher") == 0;
}

/* 这个机型有没有 L/R 肩键。判据 = **该机型真实手柄有没有 L/R**（不是核心有没有映射它 ——
 * 上游常为"按键不够"的硬件把 X/Y/L/R 重排掉，那是实现问题，不是硬件没有）。
 *   GBA ✔（A/B/L/R + START/SELECT）
 *   SNES ✔（A/B/X/Y + L/R + START/SELECT）—— 上游核心把 X/Y 借去当 START/SELECT、
 *          L/R 变成组合键；触摸屏有这些键，所以另加了一套 "Full" 预设用真键（见 main_snes.c）。
 *   GB/GBC/NES/GG/SMS/COL/PCE/Lynx/GW ✘（真实手柄没有 L/R）
 * ⚠ 只有 GBA 该有"R/L 调换"按钮：那是为了把 L/R 换到拇指够得着的位置（用户 2026-10-07 定）。 */
static inline bool rg_touch_has_shoulders(const char *console_id)
{
    if (rg_touch_is_menu(console_id))
        return true;    /* 菜单：给全功能（GBA 那套） */
    return strcmp(console_id, "gba") == 0 || strcmp(console_id, "snes") == 0;
}

/* 「X/Y ↔ L/R 调换」这颗按钮只在 **GBA** 上出现（用户 2026-10-07 定：R/L 调换是 GBA 专用）。
 * ⚠ 它和 rg_touch_has_shoulders() 是**两件事**，别绑在一起：SNES 的真实手柄有 L/R（要有那行），
 *   但它的 L/R 位置本来就顺手、不需要调换 —— 早先把两者写成一个判据，SNES 上就多出一颗按钮。 */
static inline bool rg_touch_has_swap_button(const char *console_id)
{
    if (rg_touch_is_menu(console_id))
        return true;    /* 菜单：给全功能（GBA 那套） */
    return strcmp(console_id, "gba") == 0;
}

/* 这个键在该机型下**不存在**：隐藏 = 不绘制、不命中、不进分区框。
 * ⚠ 只影响"可视与命中"；键位表（targets/tab5/touch_layout.h）本身不动 —— 它仍是坐标的单一真源，
 *   真机上要挪按键就改那张表，别在这里挪。
 *
 * 判据同样来自核心映射（2026-10-07 全量勘察）：
 *   · L/R —— 只有 GBA 用（见上）。
 *   · X/Y —— 只有 GBA 核心会读；但 **GB/GBC/NES 借它们做连发 A/B**
 *     （rg_input_apply_turbo 在 input 层合成，核心不必认识 X/Y 也能用）→ 这三台要显示。
 *   · SNES/SMS/GG/COL/PCE/GW/Lynx 一律只用到 方向 + A/B + SELECT/START，
 *     所以它们的动作区只剩 A/B 两颗（A/B 位置**不动**，保持跨机型手感一致）。 */
static inline bool rg_touch_key_hidden(const char *console_id, rg_key_t key)
{
    if (rg_touch_is_menu(console_id))
        return false;   /* 菜单：全给（GBA 那套） */
    if (key == RG_KEY_L || key == RG_KEY_R)
        return !rg_touch_has_shoulders(console_id);   /* 真实手柄有 L/R 的：GBA / SNES */
    if (key == RG_KEY_X || key == RG_KEY_Y)
    {
        /* 这四类机型要显示 X/Y，但**用途不同**：
         *   · SNES —— X/Y 是**真按键**（配 main_snes.c 的 "Full" 预设）。
         *   · GBA / GB / GBC / NES / SMS / GG —— 借 X/Y 做**连发 A/B**
         *     （rg_input_apply_turbo 在 input 层合成；SMS/GG 走 main_sms.c，COL 排除在外）。
         *   其余机型既没有 X/Y 也没有连发 → 不显示。 */
        return !(strcmp(console_id, "snes") == 0 || strcmp(console_id, "gba") == 0 ||
                 strcmp(console_id, "gb")   == 0 || strcmp(console_id, "gbc") == 0 ||
                 strcmp(console_id, "nes")  == 0 ||
                 strcmp(console_id, "sms")  == 0 || strcmp(console_id, "gg")  == 0);
    }
    return false;
}

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
