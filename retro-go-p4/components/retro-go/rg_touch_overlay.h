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

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY

#define RG_OVERLAY_ALPHA_LEVEL_COUNT 5
#define RG_OVERLAY_PRESS_LINGER_MS   120   /* 按下高亮的最短保持（快速点按也看得见） */

/* NVS 键（NS_GLOBAL）——菜单和本模块共用一份，别各写各的字面量 */
#define RG_TOUCH_SETTING_VISIBLE "TouchButtons"
#define RG_TOUCH_SETTING_ALPHA   "TouchOpacity"

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

/* 仅预览/调试用：强制某些键显示为按下（PC 截图脚本用） */
void rg_overlay_debug_set_pressed(uint32_t mask);

/* 左上角开关的命中矩形（逻辑坐标）—— 触摸层用它判断"是不是点了开关"。
 * 开关只在按键隐藏时显示，但矩形始终可查（单一数据源，别在别处再写一遍坐标）。 */
void rg_overlay_get_toggle_rect(int *x, int *y, int *w, int *h);

#endif /* RG_GAMEPAD_TOUCH_MAP && RG_TOUCH_OVERLAY */
