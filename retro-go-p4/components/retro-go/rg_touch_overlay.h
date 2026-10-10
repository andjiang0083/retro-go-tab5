/* ============================================================================
 * 虚拟按键（触摸手柄）可视层 —— 目标无关的共享实现
 * ----------------------------------------------------------------------------
 * 三层需求都在这里：
 *   ① 标签：每个按键画出可辨认的标识（十字键=三角箭头，ABXY/L/R=字母，系统键=文字）
 *   ② 透明度：5 档（100/80/60/40/20%），运行时可调、存 NVS、免刷机
 *   ③ 按下反馈：手指按到哪个键，哪个键立刻亮起来（触摸没有实体键的硬伤）
 *
 * 关键设计（为什么这么做，改之前先读）：
 *  - **一次性预渲染**：init 时把每个按键烘成一张"掩码字节图"
 *      mask 字节 = (覆盖率<<3) | 角色    覆盖率 0..16（4x4 超采样），角色 1=边框 2=填充 3=文字
 *    每帧只做一次查表混合，零三角函数、零字形解析。整屏按键合计约 123KB（掩码），
 *    比存 RGB 位图（约 750KB）省 6 倍，也省 PSRAM 带宽。
 *  - **超采样白拿抗锯齿**：形状用 4x4 子采样求覆盖率，圆角和斜边天然平滑 —— 一次性成本，
 *    每帧零开销。这是"预渲染"相对"每帧画形状"最大的红利。
 *  - **透明度分层**：填充最淡(0.50)、边框次之(0.90)、文字最实(1.00)。若三层同透明度，
 *    低档位下填充和文字一起变淡 → 字看不清；分层后 20% 档位下字仍然认得出。
 *  - **混合权重表**：acov[角色][覆盖率] 已含"档位×分层"两个系数，每像素只需查表 + 一次混合。
 *  - 颜色/标签规则改动后，请同步 tools/preview-touch-overlay.py（PC 设计稿预览）：
 *    两边规则一致才有意义。
 * ==========================================================================*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 必须先拿目标配置（RG_GAMEPAD_TOUCH_MAP / RG_SCREEN_* 都在 config.h 里），
 * 否则下面的 #if 会在配置可见之前求值 → 整个模块被编译成空。rg_system.h 有 include guard，
 * 重复包含无副作用；它内部已经 include 了 rg_input.h（键位表类型）。 */
#include "rg_system.h"

/* 这个 target 是否支持虚拟按键可视层（0 = 完全不编，回到"盲区"模式） */
#ifndef RG_TOUCH_OVERLAY
#define RG_TOUCH_OVERLAY 1
#endif

/* 「这次编译到底有没有可视层模块」的**唯一判据** —— 调用方（launcher/显示层/输入层）
 * 一律用这个宏，别写 `#if RG_TOUCH_OVERLAY`。
 * 原因：RG_TOUCH_OVERLAY 有默认值 1，而真正的开关是 RG_GAMEPAD_TOUCH_MAP（宿主/无触摸
 * target 没定义它 → 整个模块编成空）。拿 RG_TOUCH_OVERLAY 判断会「调用留着、声明被编掉」，
 * 症状是编译期 undeclared function（2026-10-03：SDL2 宿主构建就是这么坏的）。 */
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
#define RG_OVERLAY_ENABLED 1
#else
#define RG_OVERLAY_ENABLED 0
#endif

#if RG_OVERLAY_ENABLED

#define RG_OVERLAY_ALPHA_LEVEL_COUNT 5
#define RG_OVERLAY_PRESS_LINGER_MS   120   /* 按下高亮的最短保持（快速点按也看得见） */

/* NVS 键（NS_GLOBAL）——菜单和本模块共用一份，别各写各的字面量 */
#define RG_TOUCH_SETTING_VISIBLE "TouchButtons"
#define RG_TOUCH_SETTING_ALPHA   "TouchOpacity"
#define RG_TOUCH_SETTING_SWAP    "TouchSwapYX"    /* X/Y ↔ L/R 调换开关（持久化） */
#define RG_TOUCH_SETTING_SKIN    "TouchSkin"      /* 皮肤索引（4 套可切，表在 rg_touch_skin.c） */

extern const int rg_overlay_alpha_levels[RG_OVERLAY_ALPHA_LEVEL_COUNT];

/* 建层：必须在显示初始化之后调用一次（要读键位表）。
 * ⚠ 它挂在 lcd_init() 里 → **每次 app 切换（进出游戏）都要重付一次**，所以耗时直接等于
 *   黑屏等待。实测 Tab5 上 13 键约 200ms（含 PSRAM 掩码分配 + 建层）。
 *   优化前是秒级：热路径用了 double，而 P4 的 FPU 只有单精度，double 走软件模拟
 *   （详见 rg_touch_overlay.c 里 label_hit 的注释与经验沉淀 §94）——别把 double 放回去。 */
void rg_overlay_init(void);
bool rg_overlay_is_ready(void);

bool rg_overlay_get_visible(void);
void rg_overlay_set_visible(bool visible);
int  rg_overlay_get_alpha(void);                 /* 百分比 100/80/60/40/20 */
void rg_overlay_set_alpha(int percent);          /* 自动吸附到最近的档位 */
void rg_overlay_cycle_alpha(int direction);      /* +1 / -1 档，菜单用 */

/* 合成到缓冲区：
 *   rg_overlay_blit        —— 逻辑朝向（SDL2 宿主预览、任何逻辑帧缓冲的 target）
 *   rg_overlay_blit_cw90   —— tab5 物理朝向（面板原生竖屏，90°顺时针映射后的块）
 * 参数 (x,y,w,h) 是该缓冲在屏幕上的位置（逻辑坐标 / 物理坐标），stride 为行跨距（像素）。 */
void rg_overlay_blit(uint16_t *buf, int stride, int x, int y, int w, int h);
void rg_overlay_blit_cw90(uint16_t *buf, int stride, int x, int y, int w, int h, int phys_w);
void rg_overlay_blit_linear(uint16_t *buf, int stride, int x, int y, int w, int h, int phys_w);

/* 仅预览/调试用：强制某些键显示为按下（PC 截图脚本用） */
void rg_overlay_debug_set_pressed(uint32_t mask);

/* 左上角开关的命中矩形（逻辑坐标）—— 触摸层用它判断"是不是点了开关"。
 * 开关只在按键隐藏时显示，但矩形始终可查（单一数据源，别在别处再写一遍坐标）。 */
void rg_overlay_get_toggle_rect(int *x, int *y, int *w, int *h);

/* ---------------------------------------------------------------- X/Y ↔ L/R 调换
 * L/R 之间那颗切换按钮（几何在 targets/tab5/touch_layout.h 的 RG_TAB5_SWAP_*）：
 *   默认菱形位是 X/Y；点一下 → X↔R、Y↔L 对调（菱形位变 R/L，肩键位变 X/Y），
 *   标签显示"菱形上现在是哪一对"（X/Y ↔ L/R）。不是游戏按键，不注入任何 RG_KEY_*。
 * 状态存 NVS（RG_TOUCH_SETTING_SWAP），断电不丢；切完立刻重建那四个键的掩码。
 * 输入层（rg_input.c）负责命中这颗按钮并调用 set_swap()。 */
void rg_overlay_get_swap_rect(int *x, int *y, int *w, int *h);
bool rg_overlay_get_swap(void);
void rg_overlay_set_swap(bool on);

/* ---------------------------------------------------------------- 皮肤（4 套可切）
 * 皮肤 = 控制区面板（分区色块/细线/凹槽/机型铭牌）+ 每键配色。表与面板生成在
 * rg_touch_skin.c（**唯一真源**，PC 预览工具解析同一个文件）。
 * 切换**不重建掩码**：掩码里只有"覆盖率 + 角色"，颜色来自每键的 pal[] ——
 * 所以换皮肤只是重算调色板 + 原地重画面板（约 30ms 一次整屏重推），不卡。 */
int  rg_overlay_skin_count(void);
int  rg_overlay_get_skin(void);
void rg_overlay_set_skin(int idx);          /* 落定：立刻生效 + 存 NVS；面板同步重画 */
void rg_overlay_preview_skin(int idx);       /* 试穿：立刻生效但**不存 NVS**（菜单预览/回退用） */
const char *rg_overlay_skin_name(int idx);  /* 菜单显示用（ASCII） */
const char *rg_overlay_skin_short_name(int idx);
/* 把当前皮肤的面板底图交给显示层（幂等）。
 * 调用点：建层末尾 / 换皮肤 / 用户改了 Border 设置之后。
 * 用户手选了 Border 图时它会被忽略 —— 用户的选择优先。 */
void rg_overlay_refresh_panel(void);

/* 供皮肤面板生成器取 8x8 字形（同一个字库、同一份加载结果；未支持返回 NULL）。
 * 见 rg_touch_skin.c 的 panel_text() —— 铭牌文字就是这么画的。 */
const uint8_t *rg_overlay_glyph_rows(int code);

/* 每次调换 +1。给"菜单/启动器"这类**事件驱动重绘**的界面用：
 * 它们只在收到按键时才重画，而调换不产生任何按键 —— 拿这个号比一下就知道
 * 覆盖层的内容变了、该重画一帧（不然标签要等到下一次界面切换才更新）。
 * 游戏里每帧都在写画面，用不到这个。 */
uint32_t rg_overlay_get_generation(void);

/* 取走"需要整块重画"的单元矩形（取走即清）：往 out_xywh 里填 x,y,w,h 四元组，返回个数。
 *
 * 为什么需要这个：显示层的推帧循环**只覆盖视口**（游戏里 720x480），而控制条带在视口
 * 之外的黑边上 —— 条带上的叠加层（按压反馈、那颗 X/Y↔L/R 调换按钮）靠推帧永远刷不到。
 * 于是显示层每帧来问一次，拿到矩形后自己把条带的**背景**（边框图或纯黑）重建一遍，
 * 再走正常的发送路径（驱动会在发送时把叠加层合成上去）。
 * 实测证据：真机"点了不变，只有进出游戏才变"—— 因为进出游戏会触发整屏重画。
 * 上限：13 个按键单元 + 1 个切换按钮，取 RG_OVERLAY_DIRTY_MAX 够用（多出来的丢弃，不影响正确性）。 */
#define RG_OVERLAY_DIRTY_MAX 16
int rg_overlay_take_dirty_rects(int *out_xywh, int max);

/* X/Y ↔ L/R 的调换规则 —— **只此一处**（输入命中、可视层标签、可视层配色共用）：
 *   X↔R、Y↔L；A/B、十字键、系统键都不动。
 * 它是自反的（应用两次回原样），所以"调换/换回"是同一个调用。 */
static inline rg_key_t rg_touch_swap_key(rg_key_t k)
{
    switch (k)
    {
        case RG_KEY_X: return RG_KEY_R;
        case RG_KEY_R: return RG_KEY_X;
        case RG_KEY_Y: return RG_KEY_L;
        case RG_KEY_L: return RG_KEY_Y;
        default: return k;
    }
}

/* "这个**位置**现在代表哪个键"：调换开着 → X↔R、Y↔L，关着 → 原样。
 * **输入层与可视层都必须调它**，别自己写 `swap ? swap_key(k) : k` ——
 * 曾经输入层漏了状态门控（自反对换被无条件应用）→ 真机"按 X 亮 R、按 Y 亮 L"。 */
rg_key_t rg_overlay_map_key(rg_key_t position_key);

/* 屏幕上的"显示帧率"数字：画在**控制区顶部正中**（逻辑 360,500 —— L/R 肩键之间的空白带；
 * 竖屏下 y<480 是游戏画面，不能占用）。坐标与线性映射都在 rg_touch_overlay.c 里
 * （2026-09-29 走查 P2-11：原来写的是横屏口径的 640,60 + cw90 换算式，已改）。
 * 由 rg_system.c 的 update_statistics() 每秒推一次值 —— 用的是 statistics 里
 * partialFPS + fullFPS（"真正显示出去的帧率"），与日志 FPS:(跳过+部分+完整) 的后两项同口径。
 * 传负值 = 不显示。直绘，不走按键掩码。 */
/* 屏幕帧率数字（开发调试用）：
   发布版关闭，保持画面干净。需要看帧率时把 0 改成 1 重新编译即可。 */
#define RG_OVERLAY_SHOW_FPS 0

void rg_overlay_set_fps(int value);

/* ---------------------------------------------------------------- 电量圆灯
 * 用户规格：START 正上方居中、**实心小圆**（不要光晕/渐变 —— 光晕在大屏上易显塑料感）。
 * 颜色：绿 100~60% / 橙 60~20% / 红 20~10% / <10% 红闪（**硬闪**，告警要抓眼）；
 * **充电中 → 绿呼吸**（优先级最高；4 档渐变 ≈1.4s 一圈）。
 *   v0.4.1 起把"充电中"从硬闪改成呼吸：INA226 分流采样会抖，叠加硬闪会显得不自然；
 *   充电状态本身另有一次软件表决去抖（见 rg_input.c）。
 * 数据来源：rg_input_read_battery()（输入任务每 2s 更新一次的缓存值，含 charging）。
 *
 * ⚠ 它落在**控制区**（逻辑 y≈1090），而显示驱动只把游戏区（y<480）的条带推给面板，
 *   普通叠加层路径永远覆盖不到这里 —— 所以仿照帧率数字的做法：由驱动调
 *   rg_batt_led_refresh_needed() / _get_band() / _draw() 直接写面板帧缓冲并做局部
 *   cache 写回（见 mipi_dsi_tab5_p.h 里 tab5_batt_led_refresh 的注释与实测教训）。 */
bool rg_batt_led_refresh_needed(void);                          /* 样子变了才 true（含闪烁相位） */
void rg_batt_led_get_band(int *x0, int *y0, int *x1, int *y1);   /* 逻辑坐标整行带（跟随灯位，行号按 4 取整） */
uint16_t rg_batt_led_band_bg(void);                              /* 擦条带的背景色 = 当前皮肤面板底色 */
void rg_batt_led_draw(uint16_t *buf, int stride);                /* 线性映射（物理=逻辑）直绘 */
void rg_batt_led_draw_cw90(uint16_t *buf, int stride, int phys_w); /* 90CW 映射直绘（横屏驱动用） */
void rg_batt_led_get_rect(int *x0, int *y0, int *x1, int *y1);    /* 灯的紧贴逻辑矩形（横屏算物理矩形用） */

#endif /* RG_GAMEPAD_TOUCH_MAP && RG_TOUCH_OVERLAY */
