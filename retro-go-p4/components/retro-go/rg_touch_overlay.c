/* 虚拟按键可视层实现 —— 设计说明见 rg_touch_overlay.h */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rg_touch_overlay.h"
#include "rg_touch_skin.h"      /* 皮肤表/面板生成（配色与面板的唯一真源） */

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY

#include "fonts/fonts.h"
#include "rg_gui.h"
#include "rg_settings.h"
#include "rg_system.h"
#include "rg_utils.h"

#define SS           4      /* 超采样倍数（覆盖率 = SS*SS 个子样本中命中的个数，0..16） */
#define BORDER_PX    3      /* 边框宽度（逻辑像素） */

/* 分层不透明度（0..255）：填充最淡 / 边框次之 / 文字最实 —— 低档位下字仍可读 */
#define A_FILL          128
#define A_BORDER        230
#define A_LABEL         255
/* 按下态：填充提亮，边框与文字转白（"亮起来"的观感） */
#define A_FILL_PRESSED  204

#define RG_OVERLAY_MAX_BUTTONS 24   /* 按键数上限（键位表实际 13 个），越界就截断不写坏内存 */

#define AW_BITS 6                /* 混合权重位宽（见 blend_px 的说明：必须 ≤ 6） */
#define AW_ONE  (1 << AW_BITS)   /* 权重满值 = 64 */

/* 控制区顶边（逻辑像素）：皮肤面板的几何基准。
 * ⚠ 多核（GB/NES）落地后要改成"按各机型的视口高度传" —— 现在 GBA 视口 720x480 贴顶，
 *   所以是常量 480。面板的锚定规则（上三排随控制区顶下移、系统键行钉屏底）已经在
 *   rg_touch_skin.c 里按参数写好了，届时只需把这里换成一个变量。 */
#define RG_OVERLAY_CTRL_TOP 480
/* 面板物理高（Tab5 竖屏 720x1280）：控制区"整条重铺"要用它算高度。
 * ⚠ 必须与 rg_touch_skin.c 里面板生成的高度一致（那边是物理全屏尺寸）。 */
#define RG_OVERLAY_PANEL_H 1280

/* 电量圆灯几何（逻辑像素）。提到文件头部的原因：建层时机（rg_overlay_init）也要用它 ——
 * 皮肤面板的机型铭牌锚在"菱形键底 ~ 圆灯顶"的正中，所以面板生成需要这两个数。
 * 原来定义在文件后段，只能被后面的函数引用。 */
/* 灯位（两种方向各一套）：
 *   竖屏：控制区中部，x 360 = 逻辑屏半宽；y 由皮肤布局推导（rg_touch_skin_led_cy），下面的值只是兜底。
 *   横屏：**左下角留白**（用户 2026-10-08："铭牌+圆灯放左下角"）—— 那块 x[0,280) y[547,720)
 *     在所有机型的最大游戏窗之外（GB 4x 左边界 320 / 下边界 696 都不进来），
 *     也不与右下角挪过去的系统键重叠。横屏是**定点**，不跟控制区走（见 rg_overlay_try_panel）。
 * 圆灯半径/圈宽两方向共用（12/3），横屏同样大小——面板物理像素密度一样。 */
/* 0.4.9：灯位改成**运行时**表达式（同一个镜像两个方向），调用点一字不改。
 * 判据 = 逻辑画面是不是横的（等价于原来那个 #if）。 */
#define RG_BATT_LED_IS_LANDSCAPE (rg_geom()->w > rg_geom()->h)
#define RG_BATT_LED_CX       (RG_BATT_LED_IS_LANDSCAPE ? 140 : 360)
#define RG_BATT_LED_CY       (RG_BATT_LED_IS_LANDSCAPE ? 675 : 1025)    /* 竖屏兜底值：真实位置由 rg_touch_skin_led_cy() 推导（见 led_cy_cur） */
#define RG_BATT_LED_R        12
#define RG_BATT_LED_RING     3
/* 灯条带 Y 范围不再写死：跟着 led_cy_cur 走（见 rg_batt_led_get_band），
 * 行号还要按 4 取整（帧缓冲一行 1440B，128B 边界 = 每 4 行，msync 要求对齐）。 */
#define RG_BATT_LED_BAND_PAD 2

#define ROLE_NONE   0
#define ROLE_BORDER 1
#define ROLE_FILL   2
#define ROLE_LABEL  3

/* NVS 键（与 rg_gui.c 菜单共用同一份定义，见 rg_touch_overlay.h） */
#define SETTING_VISIBLE RG_TOUCH_SETTING_VISIBLE
#define SETTING_ALPHA   RG_TOUCH_SETTING_ALPHA
#define SETTING_SWAP    RG_TOUCH_SETTING_SWAP
#define SETTING_SKIN    RG_TOUCH_SETTING_SKIN

const int rg_overlay_alpha_levels[RG_OVERLAY_ALPHA_LEVEL_COUNT] = {100, 80, 60, 40, 20};

typedef struct
{
    rg_key_t key;
    int x, y, w, h;      /* 逻辑坐标：左上角 + 尺寸 */
    uint8_t *mask;       /* w*h： (覆盖率<<3) | 角色 */
    uint16_t pal[4];     /* 正常态：角色 -> 颜色 */
    uint16_t pal_p[4];   /* 按下态 */
    /* ── 调换用的"另一套"（只有 X/Y/L/R 四个键 + 那颗切换按钮有）───────────
     * 两套都在开机时预渲染好，点击只做对换（key/mask/调色板一起换）：
     *   ① 点击路径上没有分配/释放 —— 输入任务里直接改也安全（不会 free 掉
     *      显示线程正在解引用的掩码，那是原设计的隐患）；
     *   ② 不依赖"下一帧"，渲染线程下次画时读到的就是新的那一套。
     * 代价：5 个单元多一份掩码（≈2KB PSRAM）+ 开机多约 90ms 预渲染。 */
    uint8_t *mask_alt;
    uint16_t pal_alt[4];
    uint16_t pal_p_alt[4];
    rg_key_t key_alt;
} rg_overlay_btn_t;

typedef struct
{
    int kind;            /* 0=无 1=三角 2=文字 3=十字（开关图标） */
    int dir;             /* 三角朝向：0=上 1=下 2=左 3=右 */
    const char *text;
    int text_len;        /* strlen 结果缓存：热路径里每样本一次 strlen 是浪费 */
    int scale;
    float inv_scale;     /* 1/scale：热路径用乘法代替除法 */
    float tx, ty;        /* 文字左上角 */
    float cx, cy;        /* 三角中心 */
    float tri_h;         /* 三角半高 / 十字臂长 */
    float arm_w;         /* 十字臂半宽 */
    float bx0, by0, bx1, by1;   /* 标识包围盒（外扩 1px）：框外像素不必做字形采样 */
} rg_overlay_label_t;

/* 左上角"虚拟按键开关"的几何（逻辑像素）。
 * 按键隐藏时它显示、点一下把按键放回来 —— 这是隐藏状态下唯一能恢复的入口，
 * 所以必须可见（比原来那个"左上角长按 1.2s"的秘密手势强）。 */
/* 开关：命中矩形比视觉矩形外扩一圈（手指没那么准，它是隐藏态唯一入口，宁大勿小）。
 * 外扩不会压到游戏区：逻辑左边距 280px，开关连外扩只到 x=112。 */
#define RG_OVERLAY_TOGGLE_PAD 12
#define RG_OVERLAY_TOGGLE_BORDER 3         /* 边框厚度 */
#define RG_OVERLAY_TOGGLE_ARM_PCT 30       /* 十字臂长 = min(w,h) * 30% */
#define RG_OVERLAY_TOGGLE_WID_PCT 11       /* 十字臂宽 = min(w,h) * 11% */

#define RG_OVERLAY_TOGGLE_X 12
#define RG_OVERLAY_TOGGLE_Y 12
#define RG_OVERLAY_TOGGLE_W 88
#define RG_OVERLAY_TOGGLE_H 48

/* ---------------- 当前游戏视口的运行时几何（**必须在所有使用点之前**）----------------
 * 面板底图、"整条重铺"矩形、布局自检都要知道"画面到哪结束、控制区从哪开始"：
 *   GBA 240x160 @3x = 720x480 贴顶        → 控制区顶 480（控制区高 800）
 *   GB  160x144 @3x = 480x432 居中 top=24 → 控制区顶 456（控制区高 824）
 *   NES 256x240 @2x = 512x480 居中 top=0  → 控制区顶 480（控制区高 800）
 * 全部取自 rg_display_get_info()->viewport（视口在**逻辑屏 720x480** 内居中；竖屏物理下
 * 逻辑 y 与物理 y 同向，可直接用）。⚠ 别再写死 480 —— 那是"换机型后凹槽/铭牌全错位"的来源。 */
static int ctrl_top_cur = 480;   /* 兜底 = GBA 值（视口未就绪时用，与 RG_OVERLAY_CTRL_TOP 同值） */
static int win_x_cur = 0;
static int win_w_cur = 720;

static void rg_overlay_sync_viewport(void)
{
    const rg_display_t *d = rg_display_get_info();
    if (!d || d->viewport.width <= 0 || d->viewport.height <= 0)
        return;
    win_x_cur = d->viewport.left;
    win_w_cur = d->viewport.width;
    ctrl_top_cur = d->viewport.top + d->viewport.height;   /* 视口底 = 控制区顶 */
    if (ctrl_top_cur < 0 || ctrl_top_cur > 1280 - 64)
        ctrl_top_cur = 480;                                /* 明显不合理 → 退回兜底 */
}

static rg_overlay_btn_t *btns;
static size_t btn_count;
static bool ready;
static bool visible = true;
static int alpha_pct = 100;
static int alpha_level = 255;             /* 0..255，= alpha_pct * 255 / 100
                                           * ⚠ 跨任务读写：设置菜单（GUI 任务）写、显示与输入路径读，
                                           * 故意**不加锁** —— 32 位对齐 int 的读写在本平台是一条指令，
                                           * 加锁只会在显示热路径上引入优先级反转（走查 P2-16）。
                                           * 真要动这里，先想清楚"读到上一档"的后果可接受。 */
static bool pending_commit = false;       /* 设置改了但还没落盘（见 commit_if_pending） */
static uint8_t acov[4][17];               /* 正常态混合权重 [角色][覆盖率] */
static uint8_t acov_p[4][17];             /* 按下态 */
/* ⚠ 按下保持计时必须**按按钮序号**存，不能按 rg_key_t 的值存：
 * RG_KEY_* 是位掩码（RG_KEY_R = 1<<13 = 8192），拿它当下标越界 32KB —— 实机第一帧合成就
 * LoadProhibited 崩溃；宿主 .bss 邻页可读，所以 SDL2 预览完全看不出来（教训见经验沉淀 §92）。 */
static uint32_t last_press_ms[RG_OVERLAY_MAX_BUTTONS];

/* ---- X/Y ↔ L/R 调换（L/R 之间那颗切换按钮）----------------------------------
 * swap_yx      当前是否已调换（NVS: RG_TOUCH_SETTING_SWAP）
 * swap_btn     那颗按钮自己的掩码（不在 btns[] 里：它不是游戏键）
 * swap_flash_ms 上次点击时刻（点一下亮一次）
 * 颜色用 MENU 那颗琥珀 —— 调色板里琥珀专属于"系统/UI 控件"，一看就不是游戏键。 */
static bool swap_yx = false;
static rg_overlay_btn_t swap_btn;
/* ── "控制条带上哪些单元需要整块重画"（取走即清，见 rg_overlay_take_dirty_rects）──
 * 游戏里显示层只推视口（720x480），控制条带在黑边上推不到 —— 按压反馈和那颗调换按钮
 * 都靠这套标记让显示层把条带重建一遍。 */
static uint32_t dirty_units = 0;        /* bit i = btns[i] 的外观变了 */
static bool dirty_swap_btn = false;     /* 那颗切换按钮（不在 btns[] 里） */
static uint32_t dirty_last_visual = 0;  /* 上次看到的"高亮位"（含 linger 保持） */
static uint32_t dirty_last_visual_before = 0; /* 再上一拍的高亮位：给抖动观测用（只统计） */
static bool dirty_all_units = false;    /* 调换/显隐：13 个单元全重画（外观可能全变） */
/* 皮肤面板/边框整条重铺一次：Tab5 上"视口恰好等于逻辑屏"，display.changed 那条整屏重铺
 * 条件是 `视口 < 屏`，对 Tab5 不成立 —— 也就是说**面板底/凹槽/铭牌的变化没人负责刷出去**
 * （逐键的 dirty_units 只盖按键本身）。切皮肤时置这个标志，让下一帧把整条控制区刷一遍。 */
static bool dirty_panel_strip = false;
static bool swap_btn_ready = false;
/* "这个**位置**现在代表哪个键" —— 调换开着时 X↔R、Y↔L，关着时原样。
 *
 * **输入层与可视层都必须走这里**（只此一处）：曾经两边各写一份，可视层按状态门控、
 * 输入层却无条件应用自反对换（换两次回原样的性质让它"看起来也对"）→ 真机表现为
 * 默认 X/Y 模式下按 X 亮起的是 R、按 Y 亮起的是 L（而且游戏收到的是 R/L）。
 * 规则本身在 rg_touch_swap_key()（自反），**状态的消费点**就是本函数。 */
rg_key_t rg_overlay_map_key(rg_key_t position_key)
{
    return swap_yx ? rg_touch_swap_key(position_key) : position_key;
}

static uint32_t swap_flash_ms = 0;
static uint32_t swap_generation = 0;    /* 每次调换 +1：菜单循环靠它发现"该重画了" */
#define RG_OVERLAY_SWAP_PAD 10      /* 命中矩形比视觉矩形外扩（和左上角开关同一思路） */
static uint32_t visual_mask;              /* 含"高亮保持"的显示用按下掩码 */
static uint32_t debug_mask;               /* 预览用强制按下 */
static uint8_t glyphs[128][8];            /* ASCII 8x8 点阵，MSB = 最左列 */

/* ---- 皮肤（4 套可切，NVS: SETTING_SKIN）-------------------------------------
 * 掩码里只存"覆盖率 + 角色"，颜色来自每键的 pal[] ⇒ **换皮肤不重建掩码**
 * （13 键重烘焙约 200ms，切换会明显卡）；只重算调色板 + 原地重画面板。
 * console_id = 当前核心的机型 id（gba/gb/gbc/nes/…）：决定铭牌文案与 D 套的点缀色。 */
static int skin_idx = 0;
static char console_id[16] = "";
/* 圆灯中心 y（物理像素）：面板与灯条带共用**同一个推导值** —— 两边各写常量必然漂移。
 * 面板装好/换皮肤时由 rg_touch_skin_led_cy() 更新。 */
/* 0.4.9：灯位是运行时值 ⇒ **不能在静态初始化里读**（C 要求静态初始化是常量表达式）。
 * 先给竖屏兜底值，然后在 rg_overlay_init()（显示初始化时必被驱动调用）里按当前方向修正。 */
static int led_cy_cur = 1025;

/* ---------------------------------------------------------------- 颜色工具 */

static uint16_t c565(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* RGB565 逐通道缩放（num/den） */
static uint16_t c565_scale(uint16_t c, int num, int den)
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = r * num / den;
    g = g * num / den;
    b = b * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* RGB565 向白色靠拢（k/100）：标签用键色的提亮版，比纯白更有色彩层次 */
static uint16_t c565_tint(uint16_t c, int k, int den)
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r += (31 - r) * k / den;
    g += (63 - g) * k / den;
    b += (31 - b) * k / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* 每个按键的键色。颜色不再承担"身份标识"（现在有文字了），所以按主机惯例调和：
 * 十字键归一为一个冷灰蓝的十字单元，ABXY 用主机四色，肩键浅灰，系统键中性/MENU 琥珀。
 * 注释里的 #RRGGBB 是原始 RGB888（方便和预览脚本对照）。 */
static uint16_t overlay_key_color(rg_key_t key)
{
    /* 颜色全部来自皮肤表（rg_touch_skin.c）：改动配色只需要动那张表，
     * PC 预览工具解析同一张表 —— 别在这里写死颜色（会重新长出"两边各一份"的漂移）。 */
    return rg_touch_skin_key_color(skin_idx, rg_touch_skin_slot_for_key(key), console_id);
}

/* 标签颜色覆盖（C 套统一 #FFD98A）：返回 false 表示"用 tint(键色, label%)" */

/* 每键调色板配方：把"键色 + 皮肤配方"算成 pal/pal_p 两组色。
 * 掩码不动（形状已烘好），所以这条路径就是换皮肤的全部代价。 */
static void apply_recipe(uint16_t pal[4], uint16_t pal_p[4], uint16_t color)
{
    int radius_pct = 18, fill_pct = 55, label_pct = 70;
    bool outline_only = false;
    rg_touch_skin_button_recipe(skin_idx, &radius_pct, &fill_pct, &label_pct, &outline_only);
    uint16_t label_override = 0;
    const bool has_override = rg_touch_skin_label_color(skin_idx, &label_override);

    pal[ROLE_BORDER] = color;
    /* 填充比边框暗一些：否则"亮边框+亮标签"压在亮填充上对比不足（原来的 55% 口径） */
    pal[ROLE_FILL]   = c565_scale(color, fill_pct, 100);
    pal[ROLE_LABEL]  = has_override ? label_override : c565_tint(color, label_pct, 100);
    /* 按下态：边框与文字转白，填充提亮（"亮起来"的观感） */
    pal_p[ROLE_BORDER] = 0xFFFF;
    pal_p[ROLE_FILL]   = c565_scale(color, 95, 100);
    pal_p[ROLE_LABEL]  = 0xFFFF;
}

const uint8_t *rg_overlay_glyph_rows(int code)
{
    if (code <= 0 || code >= 128)
        return NULL;
    return glyphs[code][0] ? glyphs[code] : NULL;
}

/* ---------------------------------------------------------------- 字形 */

static void load_glyphs(void)
{
    const uint8_t *p = font_basic8x8.data;
    for (int guard = 0; guard < 512; ++guard)
    {
        const rg_font_glyph_t *g = (const rg_font_glyph_t *)p;
        if (!g->code)
            break;
        /* 推进长度 = **height**（每行一个字节），不是按位紧凑打包！
         * 这是字库的真实排法，也是 PC 预览工具 parse_font() 的口径 —— 两边必须一致，
         * 否则会出现"图上文字镜像/乱码"那类故障（2026-09-29 已踩过一次）。
         * ⚠ 历史：这里曾写成 (((w*h)-1)/8)+1（按位紧凑）。当前字库 191 个字形全是
         * 8x8，两种算法恰好等价才没出事；一旦混入 width<8 的字形，后续字形会整体错位。
         * 走查 P1-2，见 docs/archive/CODE-REVIEW-v0.4.1.md。 */
        const size_t nbytes = g->height;
        if (g->code < 128 && g->width == 8 && g->height == 8)
            for (int y = 0; y < 8; ++y)
                glyphs[g->code][y] = (uint8_t)(g->data[y] >> g->xOffset);  /* xOffset: 右移列 */
        else if (g->width != 8 || g->height != 8)
        {
            /* 非 8x8 的字形本实现不会渲染（掩码固定按 8x8 用）。推进长度虽然按
             * height 算对了，但"跳过不画"这件事必须说出来 —— 否则屏幕上少个字
             * 会像渲染坏了，而不是"字库里有不支持的尺寸"。 */
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                RG_LOGW("glyphs: %dx%d glyph U+%04X unsupported (only 8x8 is rendered)\n",
                        g->width, g->height, (unsigned)g->code);
            }
        }
        p += sizeof(rg_font_glyph_t) + nbytes;
    }
}

/* ---------------------------------------------------------------- 标签几何 */

/* 文字标签（从 label_geom 里拆出来的独立入口）：那颗调换按钮没有 rg_key_t，
 * 标签是运行时的字符串（"X/Y" / "L/R"），需要和按键走同一套排版与掩码。 */
static void label_geom_text(int w, int h, const char *text, rg_overlay_label_t *L)
{
    memset(L, 0, sizeof(*L));
    if (!text || !*text)
        return;

    int n = (int)strlen(text);
    int sx = (w * 80 / 100) / (n * 8);   /* 横向：留 20% 边距能放下的最大整数倍 */
    int sy = (h * 45 / 100) / 8;         /* 纵向：留 45% 高度 */
    int s = sx < sy ? sx : sy;
    if (s < 1) s = 1;
    L->kind = 2;
    L->text = text;
    L->text_len = n;
    L->scale = s;
    L->inv_scale = 1.0f / (float)s;
    L->tx = (w - n * 8 * s) * 0.5f;
    L->ty = h * 0.5f - 3.5f * s;         /* 字形墨迹占 7 行，按墨迹居中（不是按 8 行格） */
    L->bx0 = L->tx - 1.0f;
    L->by0 = L->ty - 1.0f;
    L->bx1 = L->tx + (float)(n * 8 * s) + 1.0f;
    L->by1 = L->ty + (float)(8 * s) + 1.0f;
}

static void label_geom(int w, int h, rg_key_t key, rg_overlay_label_t *L)
{
    memset(L, 0, sizeof(*L));

    /* 开关（左上角那个"把按键放回来"的图标）不是按键：它由 blit_toggle 程序化绘制，
     * 不进键位表，所以这里没有 toggle 分支（曾经的 is_toggle 字段已删除，走查 P2-4）。 */

    const char *text = NULL;
    int dir = -1;
    switch (key)
    {
        case RG_KEY_UP:     dir = 0; break;
        case RG_KEY_DOWN:   dir = 1; break;
        case RG_KEY_LEFT:   dir = 2; break;
        case RG_KEY_RIGHT:  dir = 3; break;
        case RG_KEY_A:      text = "A"; break;
        case RG_KEY_B:      text = "B"; break;
        case RG_KEY_X:      text = "X"; break;
        case RG_KEY_Y:      text = "Y"; break;
        case RG_KEY_L:      text = "L"; break;
        case RG_KEY_R:      text = "R"; break;
        case RG_KEY_SELECT: text = "SELECT"; break;
        case RG_KEY_START:  text = "START"; break;
        case RG_KEY_MENU:   text = "MENU"; break;
        default: break;
    }

    if (dir >= 0)
    {
        /* 三角箭头：随按键尺寸自适应（比字符更清晰，且不受 8px 点阵粒度限制） */
        int t = (w < h ? w : h) * 46 / 100;
        if (t < 8) t = 8;
        L->kind = 1;
        L->dir = dir;
        L->cx = w * 0.5f;
        L->cy = h * 0.5f;
        L->tri_h = t * 0.5f;
        L->bx0 = L->cx - L->tri_h - 1.0f;
        L->by0 = L->cy - L->tri_h - 1.0f;
        L->bx1 = L->cx + L->tri_h + 1.0f;
        L->by1 = L->cy + L->tri_h + 1.0f;
        return;
    }

    if (text)
        label_geom_text(w, h, text, L);
}

/* ⚠ 这个函数在预渲染热路径里被调用百万次，必须保持 float（P4 的 FPU 只有单精度，
 * double 走软件模拟慢几十倍）且不能有除法/strlen（每样本一次除法 = 建层从 10ms 变 2s）。 */
static bool label_hit(const rg_overlay_label_t *L, float x, float y)
{
    if (L->kind == 1)
    {
        float h = L->tri_h;
        float dx = x - L->cx, dy = y - L->cy, u, v;
        switch (L->dir)
        {
            case 0:  u =  dx; v =  dy; break;   /* 上 */
            case 1:  u =  dx; v = -dy; break;   /* 下 */
            case 2:  u =  dy; v =  dx; break;   /* 左 */
            default: u =  dy; v = -dx; break;   /* 右 */
        }
        if (v < -h || v > h)
            return false;
        return (u < 0 ? -u : u) <= (v + h) * 0.5f;
    }

    if (L->kind == 3)
    {
        float dx = x - L->cx, dy = y - L->cy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return (dx <= L->arm_w && dy <= L->tri_h) || (dy <= L->arm_w && dx <= L->tri_h);
    }

    if (L->kind == 2)
    {
        float lx = (x - L->tx) * L->inv_scale;
        float ly = (y - L->ty) * L->inv_scale;
        if (lx < 0 || ly < 0 || ly >= 8)
            return false;
        int cx = (int)lx, cy = (int)ly;
        int ci = cx >> 3, gx = cx & 7;
        if (ci >= L->text_len)
            return false;
        unsigned char ch = (unsigned char)L->text[ci];
        if (ch >= 128)
            return false;
        return (glyphs[ch][cy] & (0x80 >> gx)) != 0;
    }

    return false;
}

/* 圆角矩形内部判定（点用像素中心坐标） */
static bool in_rrect(float x, float y, float w, float h, float r)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return false;
    if (r <= 0)
        return true;
    float cx = x < r ? r : (x > w - r ? w - r : x);
    float cy = y < r ? r : (y > h - r ? h - r : y);
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

/* ---------------------------------------------------------------- 建层 */

/* text_override != NULL 时用它当标签（那颗调换按钮不是游戏键，没有 rg_key_t 可查）。 */
static void build_button(rg_overlay_btn_t *b, uint16_t color, const char *text_override)
{
    const int w = b->w, h = b->h;
    rg_overlay_label_t L;
    if (text_override)
        label_geom_text(w, h, text_override, &L);
    else
        label_geom(w, h, b->key, &L);

    int radius_pct = 18, fill_pct = 55, label_pct = 70;
    bool outline_only = false;
    rg_touch_skin_button_recipe(skin_idx, &radius_pct, &fill_pct, &label_pct, &outline_only);

    float r = (float)((w < h ? w : h) * radius_pct / 100);
    if (r < 2) r = 2;
    float bw = BORDER_PX;
    if (bw * 2 > (w < h ? w : h) / 2) bw = (w < h ? w : h) / 4.0f;

    b->mask = rg_alloc((size_t)w * h, MEM_SLOW);
    if (!b->mask)
    {
        RG_LOGE("touch overlay: no memory for button %dx%d\n", w, h);
        return;
    }

    /* 颜色与配方全部来自皮肤表（rg_touch_skin.c）：这里只剩"算"，没有"常量" */
    apply_recipe(b->pal, b->pal_p, color);

    /* 内层（填充区）在外层坐标里内缩 bw、圆角 r-bw —— 与原来 in_rrect 的形状定义一致 */
    const float iw = (float)w - 2 * bw, ih = (float)h - 2 * bw;
    float ir = r - bw;
    if (ir < 0) ir = 0;
    /* 采样点相对像素中心最大偏移 0.375，对角最大 0.53 —— 取 0.8 做保守裕量：
     * 离边界 ≥0.8px 的像素直接给满/零覆盖，结果与 4x4 采样逐位一致，但省掉 16 次采样。
     * 按键周长约 400px，也就是说整屏只有 ~1% 的像素需要真采样。 */
    const float M = 0.8f;
    const float ro_lo = (r > M ? r - M : 0.0f), ro_hi = r + M;
    const float ri_lo = (ir > M ? ir - M : 0.0f), ri_hi = ir + M;
    const float d2_ro_lo = ro_lo * ro_lo, d2_ro_hi = ro_hi * ro_hi;
    const float d2_ri_lo = ri_lo * ri_lo, d2_ri_hi = ri_hi * ri_hi;
    const int full = SS * SS;

    for (int y = 0; y < h; ++y)
    {
        const float py = y + 0.5f;
        /* 外层圆角矩形：钳制中心到内核矩形（= in_rrect 的同一形状），得到外正内负距离 */
        const float oc_y = py < r ? r : (py > (float)h - r ? (float)h - r : py);
        const float ody = py - oc_y;
        const float ipy = py - bw;
        const float ic_y = ipy < ir ? ir : (ipy > ih - ir ? ih - ir : ipy);
        const float idy = ipy - ic_y;

        for (int x = 0; x < w; ++x)
        {
            const float px = x + 0.5f;
            const float oc_x = px < r ? r : (px > (float)w - r ? (float)w - r : px);
            const float odx = px - oc_x;
            const float od2 = odx * odx + ody * ody;

            if (od2 >= d2_ro_hi)
            {
                b->mask[(size_t)y * w + x] = ROLE_NONE;   /* 完全在外层之外 */
                continue;
            }

            const float ipx = px - bw;
            const float ic_x = ipx < ir ? ir : (ipx > iw - ir ? iw - ir : ipx);
            const float idx_ = ipx - ic_x;
            const float id2 = idx_ * idx_ + idy * idy;

            int cl = 0, cf = 0, cb = 0;

            if (od2 <= d2_ro_lo && id2 <= d2_ri_lo)
            {
                /* 完全落在填充区内部：矩形覆盖必定是满的，只有标识可能在这像素内有变化 */
                if (L.kind && px >= L.bx0 && px <= L.bx1 && py >= L.by0 && py <= L.by1)
                {
                    for (int sy = 0; sy < SS; ++sy)
                        for (int sx = 0; sx < SS; ++sx)
                            if (label_hit(&L, x + (sx + 0.5f) / SS, y + (sy + 0.5f) / SS))
                                cl++;
                    cf = full - cl;
                }
                else
                {
                    cf = full;
                }
            }
            else if (od2 <= d2_ro_lo && id2 >= d2_ri_hi)
            {
                cb = full;                                /* 完全在边框带里 */
            }
            else
            {
                /* 边界带（圆角/边框内外沿/标识边缘）：老实 4x4 采样 */
                for (int sy = 0; sy < SS; ++sy)
                {
                    const float spy = y + (sy + 0.5f) / SS;
                    for (int sx = 0; sx < SS; ++sx)
                    {
                        const float spx = x + (sx + 0.5f) / SS;
                        if (!in_rrect(spx, spy, (float)w, (float)h, r))
                            continue;
                        if (in_rrect(spx - bw, spy - bw, iw, ih, ir))
                        {
                            if (L.kind && label_hit(&L, spx, spy))
                                cl++;
                            else
                                cf++;
                        }
                        else
                        {
                            cb++;
                        }
                    }
                }
            }
            uint8_t v = ROLE_NONE;
            if (cl)      v = (uint8_t)((cl << 3) | ROLE_LABEL);
            else if (cf) v = (uint8_t)((cf << 3) | ROLE_FILL);
            else if (cb) v = (uint8_t)((cb << 3) | ROLE_BORDER);
            b->mask[(size_t)y * w + x] = v;
        }
    }
}

/* 混合权重表：把"档位 × 分层"两个系数烘进查表，每像素只剩一次查表 + 一次混合。
 * 输出是 0..64 的 6 位权重（见 blend_px 的说明）。 */
static void update_acov(void)
{
    /* 三个分层权重来自皮肤表：B 套是线框皮肤（填充层权重 0），不能写死成编译期常量。 */
    uint8_t skin_tier[3];
    rg_touch_skin_tiers(skin_idx, skin_tier);
    const uint8_t tier[4]  = {0, skin_tier[0], skin_tier[1], skin_tier[2]};
    /* 线框皮肤按下时补一点淡填充：否则"按下"只有描边/文字变白，反馈偏弱 */
    const uint8_t pressed_fill = skin_tier[1] ? A_FILL_PRESSED : 72;
    const uint8_t tierp[4] = {0, A_LABEL, pressed_fill, A_LABEL};
    const int a6 = alpha_level * AW_ONE / 255;
    for (int r = 0; r < 4; ++r)
    {
        const int t6 = tier[r] * AW_ONE / 255;
        const int tp6 = tierp[r] * AW_ONE / 255;
        for (int c = 0; c <= 16; ++c)
        {
            acov[r][c]   = (uint8_t)((c * t6 * a6 + 512) / 1024);   /* /(16*64) 并四舍五入 */
            acov_p[r][c] = (uint8_t)((c * tp6 * a6 + 512) / 1024);
        }
    }
}

/* 调换按钮的标签 = "菱形位上现在是谁"（默认 X/Y；调换后变成 L/R）——
 * 用户原话："点击后，这个切换按钮应该变为 l/r"，也就是当状态指示用。 */
static const char *swap_label(void)
{
    return swap_yx ? "L/R" : "X/Y";
}

/* 备用（调换后）那套掩码还没建？见 rg_overlay_init 末尾的说明与 rg_overlay_ensure_variants()。 */
static bool variants_pending = false;

void rg_overlay_init(void)
{
    if (ready)
        return;

    /* 0.4.9：灯位是运行时值（横竖各一套）⇒ 在这里按当前方向给初值 —— 静态初始化给不了
     * （C 要求静态初始化是常量表达式）。竖屏后续会被 rg_touch_skin_led_cy() 的推导覆盖；
     * 横屏是定点，就用这个值。 */
    led_cy_cur = RG_BATT_LED_CY;

    /* ⚠ 先把用户设置读进来，再碰任何可能失败的分配：
     * 一旦下面某步失败（PSRAM 分配等）而这里没读到，visible 会停在默认 true
     * → 叠加层什么都不画（看起来"按键已关闭"）但触摸命中照旧生效 —— 这就是
     *   "按键不显示、点上去却还有反应"的根因（经验沉淀 §97）。 */
    /* ⚠ 暂时强制常显（忽略 NVS 里的开关值）：蓝牙手柄支持之前，触摸按键是这台设备
     * 唯一的输入源。用户一旦关掉它，就再没有按键能进菜单打开它（左上角那个开关真机
     * 反馈"点不动"，先放一边）——等于把设备锁死。等蓝牙手柄能用了再放开。 */
    visible = true;
    rg_overlay_set_alpha((int)rg_settings_get_number(NS_GLOBAL, SETTING_ALPHA, 100));
    /* X/Y ↔ L/R 调换（持久化）：必须在建掩码之前读进来，标签/配色才能一次到位 */
    swap_yx = rg_settings_get_boolean(NS_GLOBAL, SETTING_SWAP, false);
    /* 皮肤（持久化）：同样必须在 build_button 之前读进来 —— 配色/圆角/图层权重
     * 全按它算。默认 D 套（"主机配色点缀"）。 */
    skin_idx = rg_touch_skin_clamp((int)rg_settings_get_number(NS_GLOBAL, SETTING_SKIN, RG_TOUCH_SKIN_DEFAULT));
    {
        /* 机型 id（configNs: gba/gb/gbc/nes/…）决定铭牌文案与 D 套的点缀色 */
        const rg_app_t *app = rg_system_get_app();
        strncpy(console_id, (app && app->configNs) ? app->configNs : "", sizeof(console_id) - 1);
        console_id[sizeof(console_id) - 1] = 0;
    }

    size_t n = 0;
    const rg_keymap_touch_t *map = rg_input_get_touch_keymap(&n);
    if (!map || !n)
    {
        RG_LOGW("touch overlay: no touch keymap, disabled\n");
        return;
    }

    const int64_t t_start = rg_system_timer();

    load_glyphs();

    btns = rg_alloc(sizeof(rg_overlay_btn_t) * n, MEM_SLOW);
    if (!btns)
    {
        RG_LOGE("touch overlay: no memory for %u buttons\n", (unsigned)n);
        return;
    }
    memset(btns, 0, sizeof(rg_overlay_btn_t) * n);
    /* ⚠ btn_count 必须等于**实际建出来的**按键数：dirty 位、命中、绘制都按它对齐。
     * 该机型没有的键直接跳过（GB/GBC 无 L/R 肩键），所以用独立计数器，不能直接用 n。 */
    btn_count = 0;

    for (size_t i = 0; i < n && btn_count < RG_OVERLAY_MAX_BUTTONS; ++i)
    {
        if (rg_touch_key_hidden(console_id, map[i].key))
            continue;
        rg_overlay_btn_t *b = &btns[btn_count++];
        /* 位置固定、**功能键可调换**：b->key 存的是"这个位置现在代表哪个键"
         * （调换后菱形位上是 R/L，肩键位上是 X/Y）。按下高亮、标签、配色都读 b->key
         * → 视觉与手感永远一致；也正因为存的是功能键，高亮匹配不用额外改。
         * ⚠ 这里必须走 rg_overlay_map_key()：输入层调的是同一个函数，
         * 两边不可能再出现"一边门控、一边没门控"的不一致（真机曾按 X 亮 R）。 */
        b->key = rg_overlay_map_key(map[i].key);
        b->w = map[i].w;
        b->h = map[i].h;
        b->x = map[i].x - map[i].w / 2;
        b->y = map[i].y - map[i].h / 2;
        build_button(b, overlay_key_color(b->key), NULL);
    }

    /* L/R 之间那颗「X/Y ↔ L/R 调换」按钮：几何来自键位表（RG_TAB5_SWAP_*），
     * key=0（不是游戏键，不注入输入），颜色走皮肤的 SWAP 槽（= 系统/UI 控件色）。 */
    /* ⚠ 判据是"**GBA 专用**"（用户 2026-10-07 定），**不是** has_shoulders —— SNES 有 L/R 行
     * 但不需要调换；早先绑在一起，SNES 顶上就多出这颗按钮。 */
    if (rg_touch_has_swap_button(console_id))
    {
        swap_btn.key = RG_KEY_NONE;
        swap_btn.w = RG_TAB5_SWAP_W;
        swap_btn.h = RG_TAB5_SWAP_H;
        swap_btn.x = RG_TAB5_SWAP_X - RG_TAB5_SWAP_W / 2;
        swap_btn.y = RG_TAB5_SWAP_Y - RG_TAB5_SWAP_H / 2;
        build_button(&swap_btn, rg_touch_skin_key_color(skin_idx, RG_SKIN_SLOT_SWAP, console_id), swap_label());
        swap_btn_ready = (swap_btn.mask != NULL);
    }
    else
    {
        swap_btn.mask = NULL;
        swap_btn.mask_alt = NULL;
        swap_btn_ready = false;
    }

    /* ── 调换用的"另一套"掩码（预渲染，点击只对换）─────────────────────────
     * 只给 X/Y/L/R 四个键和这颗切换按钮建；集合在调换下闭合，所以"另一套"就是
     * 同一个位置的另一个键（含它的标签与配色）。
     * 2026-09-29：**改为延迟构建**。它占建层耗时约 1/3（真机 314ms 里约 99ms），
     * 而用户不点调换键就永远用不到 —— 开机先只建基础那套，备用那套交给
     * rg_overlay_ensure_variants() 在第一次真正需要时构建（= 玩家第一次点调换键）。
     * 不起线程、不加锁：只有一个调用点（点调换键那条路），靠这个标记做到幂等。 */
    variants_pending = true;

    /* ── 键位表自检（移植到新机型时最省事的一道保险）───────────────────────────
     * 两条规矩都是真机踩出来的：
     *   ① 命中区之间不能重叠 —— 否则手指压在两键交界处会串键，症状是"按上键却出下键"
     *      （gywan94/tab5-vgbanext 的 odroid_vpad.c 是**故意**让十字区重叠的，
     *       照抄它就会照旧串键，见 touch_layout.h 的注释）；
     *   ② 不能压进游戏画面 —— 竖屏视口在顶部（Tab5 是 y=0..480），压上去会盖住画面。
     * 只在这两条上打 WARNING，正常布局一个字都不打。移植时改完 touch_layout.h
     * 开机看一眼日志就知道有没有踩坑，不用等真机上摸出来。
     * 检查的是 build_button() 算出的**真实矩形**（中心展开 -w/2/-h/2 之后），与判定一致。 */
    {
        int bad = 0;
        for (size_t i = 0; i < btn_count; ++i)
        {
            /* ⚠ 画面高度**按当前视口**算，别写死 480：GB 3x 时视口只到 y=456，
             *   写死 480 会把"487 处的上三排"误判成没压画面（差 24px 的假阴性）。 */
            if (btns[i].y < ctrl_top_cur)
            {
                RG_LOGW("touch layout: button #%u top y=%d 压进游戏画面（视口底 %d）\n",
                        (unsigned)i, btns[i].y, ctrl_top_cur);
                bad++;
            }
            for (size_t j = i + 1; j < btn_count; ++j)
            {
                const bool ox = btns[i].x < btns[j].x + btns[j].w && btns[j].x < btns[i].x + btns[i].w;
                const bool oy = btns[i].y < btns[j].y + btns[j].h && btns[j].y < btns[i].y + btns[i].h;
                if (ox && oy)
                {
                    RG_LOGW("touch layout: 命中区重叠 #%u/#%u —— 交界处会串键\n",
                            (unsigned)i, (unsigned)j);
                    bad++;
                }
            }
        }
        if (bad)
            RG_LOGW("touch layout: 共 %d 处违规，规矩见 docs/PORTRAIT-TOUCH-LAYOUT-TEMPLATE.md\n", bad);
    }

    ready = true;
    /* 控制区面板（皮肤底图）不在这里装：这时 lcd_init 还没转屏（显示层报横屏 720x480），
     * 装了会被尺寸守卫拒绝。改到"第一次真正渲染控制区"时装（rg_overlay_try_panel）。 */
    /* 这一行会出现在串口上：app 切换（进出游戏）时 lcd_init 里会重跑建层，
     * 耗时直接决定黑屏等待时长 —— 所以别把 double/除法/strlen 放回热路径。 */
    RG_LOGI("touch overlay ready: %u buttons, visible=%d, alpha=%d%%, X/Y<->L/R swapped=%d, skin=%s, built in %d ms\n",
            (unsigned)btn_count, visible, alpha_pct, swap_yx, rg_touch_skin_short_name(skin_idx),
            (int)((rg_system_timer() - t_start) / 1000));
}

bool rg_overlay_is_ready(void)
{
    return ready;
}

/* ---------------------------------------------------------------- 运行时状态 */

bool rg_overlay_get_visible(void)
{
    return visible;
}

void rg_overlay_set_visible(bool value)
{
    if (visible == value)
        return;
    visible = value;
    rg_settings_set_boolean(NS_GLOBAL, SETTING_VISIBLE, value);
    pending_commit = true;   /* 立刻生效、延后落盘（落盘点见 commit_if_pending） */
    dirty_all_units = true;  /* 显隐切换：条带整块重画（游戏里推帧到不了条带） */
    RG_LOGI("touch overlay %s\n", value ? "shown" : "hidden");
}

int rg_overlay_get_alpha(void)
{
    return alpha_pct;
}

/* ---------------------------------------------------------------- 皮肤 API
 * 切换皮肤 = 重算调色板 + 原地重画面板 + 整片控制区置脏。
 * **不重建掩码**（掩码只记覆盖率与角色）：13 键重烘焙约 200ms，切换会明显卡；
 * 而掩码与颜色本来就是两组独立字段（当年 X/Y↔L/R 调换就是"换表不重建"的同一招）。 */
int rg_overlay_skin_count(void)
{
    return rg_touch_skin_count();
}

int rg_overlay_get_skin(void)
{
    return skin_idx;
}

const char *rg_overlay_skin_name(int idx)
{
    return rg_touch_skin_name(idx);
}

const char *rg_overlay_skin_short_name(int idx)
{
    return rg_touch_skin_short_name(idx);
}

/* 面板底图**什么时候装**：必须等 lcd_init 完成转屏（转屏后屏是 720x1280；在那之前
 * 显示层报的是横屏 720x480，面板尺寸对不上会被守卫拒绝 —— 真机日志见过
 * "内存面板尺寸不符（720x1280，应为 720x480），忽略"）。所以不在建层时装，
 * 而是**第一次真正渲染控制区时**装（那时必然已转屏）；幂等，成功后不再重复。 */
static bool panel_installed = false;
/* 已装面板对应的视口（换机型/换分辨率时判断"要不要重装"）。-1 = 还没装过。 */
static int panel_vp_x = -1, panel_vp_w = -1, panel_vp_top = -1;

/* ---------------- 当前游戏视口的运行时几何 ----------------
 * 定义在**文件顶部**（见 rg_overlay_sync_viewport 附近）：布局自检那个更早的函数也要用它，
 * 放这里会"未声明先用"。 */

static void rg_overlay_try_panel(void)
{
    if (!btns)
        return;
    /* 该不该装：**判"在不在游戏里"**，别拿 rg_display_get_width/height 去比 720x1280 ——
     * 那两个是**逻辑视口**尺寸（Tab5 上逻辑屏就是 720x480），比 1280 永远不成立。
     * 真机现象就是"面板一直不装、守卫静默挡掉、无日志"。
     * 控制区面板铺的是"游戏视口 + 下方控制条带"这个结构，启动器另有自己的全屏 UI。 */
    if (!console_id[0] || strcmp(console_id, "launcher") == 0)
        return;

    rg_overlay_sync_viewport();
    /* 视口变了（换机型 / 换分辨率）= 面板底图与"整条重铺"矩形都得重画 → 卸掉重装。
     * 同视口重复调用是幂等的（rg_touch_panel_get 内部按参数缓存，不会重复生成）。 */
    if (panel_installed && (panel_vp_x != win_x_cur || panel_vp_w != win_w_cur || panel_vp_top != ctrl_top_cur))
    {
        RG_LOGI("touch skin: viewport changed (x=%d w=%d top=%d) → (x=%d w=%d top=%d), re-installing panel\n",
                panel_vp_x, panel_vp_w, panel_vp_top, win_x_cur, win_w_cur, ctrl_top_cur);
        panel_installed = false;
    }
    if (panel_installed)
        return;

    /* led_cy 传 0 = 让皮肤布局**自己推导**（圆灯与铭牌在两排之间"各占一半、各自居中"）。
     * 以前钉死 1025 是为了跟真机对齐，但那样铭牌只能挤在灯上方，分不均。 */
    rg_surface_t *panel = rg_touch_panel_get(skin_idx, console_id, ctrl_top_cur,
                                             win_x_cur, win_w_cur, 0, RG_BATT_LED_R);
    if (!panel)
        return;
    rg_display_set_border_surface(panel);
    rg_display_border_refresh();
    panel_installed = true;
    panel_vp_x = win_x_cur; panel_vp_w = win_w_cur; panel_vp_top = ctrl_top_cur;
    dirty_panel_strip = true;   /* 让下一帧把整条控制区重铺一遍（见 dirty_panel_strip 的说明） */
    /* 灯位从布局推导（与面板同源）。条带跟着它走，所以灯动条带也动。 */
    /* 灯位从布局推导（与面板同源）。**横屏例外**：横屏的灯钉在左下角（RG_BATT_LED_CY），
     * 套竖屏那套"在控制区里居中"的推导会把它算到游戏画面附近去（用户 2026-10-08 指定左下角）。 */
    if (RG_SCREEN_WIDTH <= RG_SCREEN_HEIGHT)
        led_cy_cur = rg_touch_skin_led_cy(ctrl_top_cur, RG_BATT_LED_R);
    /* 这一行是"面板到底装上了没"的唯一判据（真机日志）：装上=控制区有底图；
     * 没有这行=被上面的判据挡了（btns 空 / 启动器），或面板生成失败。
     * 中间三个数是**本次安装用的视口**：GBA x=0 w=720 top=480 / GB x=120 w=480 top=456 /
     * NES x=104 w=512 top=480 —— 换机型时先看这行对不对得上。 */
    RG_LOGI("touch skin: panel installed for '%s' (skin %s), vp x=%d w=%d ctrl_top=%d, screen %dx%d\n",
            console_id, rg_touch_skin_name(skin_idx), win_x_cur, win_w_cur, ctrl_top_cur,
            rg_display_get_width(), rg_display_get_height());
}

void rg_overlay_refresh_panel(void)
{
    panel_installed = false;    /* 换皮肤/换 Border 设置后允许重装 */
    rg_overlay_try_panel();
}

/* "试穿"开关：菜单里光标移动到哪套就预览哪套时置 true —— 改内存、不落盘，
 * 用户按 B 退出去时还来得及干净还原（设置项不该被"逛一圈"改掉）。 */
static bool skin_preview_only = false;

/* 试穿：只改内存 + 重算调色板 + 重画面板，**不写 NVS**。 */
void rg_overlay_preview_skin(int idx)
{
    skin_preview_only = true;
    rg_overlay_set_skin(idx);
    skin_preview_only = false;
}

void rg_overlay_set_skin(int idx)
{
    idx = rg_touch_skin_clamp(idx);
    /* 落盘必须在 early-return **之前**：用户按 A 确认时，光标停的那套正是预览已经生效的
     * 那套（idx == skin_idx）——若把写 NVS 放在后面，确认会被跳过，重启就回到旧的（真机现象：
     * "选了皮肤但确定不了/不生效"的另一半原因）。 */
    if (!skin_preview_only)
        rg_settings_set_number(NS_GLOBAL, SETTING_SKIN, idx);
    if (idx == skin_idx && ready)
        return;

    skin_idx = idx;

    /* 每键调色板重算（正常态 + 按下态 + 调换那套）。掩码一个字都不动。 */
    for (size_t i = 0; i < btn_count; ++i)
    {
        apply_recipe(btns[i].pal, btns[i].pal_p, overlay_key_color(btns[i].key));
        if (btns[i].mask_alt)
            apply_recipe(btns[i].pal_alt, btns[i].pal_p_alt, overlay_key_color(btns[i].key_alt));
    }
    if (swap_btn_ready)
    {
        const uint16_t c = rg_touch_skin_key_color(skin_idx, RG_SKIN_SLOT_SWAP, console_id);
        apply_recipe(swap_btn.pal, swap_btn.pal_p, c);
        if (swap_btn.mask_alt)
            apply_recipe(swap_btn.pal_alt, swap_btn.pal_p_alt, c);
    }

    /* 图层权重也随皮肤变（B 套是线框：填充层权重 0）—— 换完重算一次 */
    update_acov();

    /* 重画：控制区整片置脏（按键高亮层）+ 面板原地重画并整张重铺 */
    dirty_all_units = true;
    rg_overlay_refresh_panel();

    RG_LOGI("touch skin: switched to %s (%d/%d)\n", rg_touch_skin_short_name(skin_idx),
            skin_idx + 1, rg_touch_skin_count());
}

void rg_overlay_set_alpha(int percent)
{
    /* 吸附到最近的档位（设置里只有 5 档，避免出现 37% 这种存不住的值） */
    int best = rg_overlay_alpha_levels[0];
    int best_d = 1000;
    for (int i = 0; i < RG_OVERLAY_ALPHA_LEVEL_COUNT; ++i)
    {
        int d = abs(percent - rg_overlay_alpha_levels[i]);
        if (d < best_d)
        {
            best_d = d;
            best = rg_overlay_alpha_levels[i];
        }
    }
    const bool changed = (best != alpha_pct);
    alpha_pct = best;
    alpha_level = best * 255 / 100;
    update_acov();
    rg_settings_set_number(NS_GLOBAL, SETTING_ALPHA, best);
    if (changed)
        pending_commit = true;
}

void rg_overlay_cycle_alpha(int direction)
{
    int idx = 0;
    for (int i = 0; i < RG_OVERLAY_ALPHA_LEVEL_COUNT; ++i)
        if (rg_overlay_alpha_levels[i] == alpha_pct)
            idx = i;
    idx += direction;
    if (idx < 0)
        idx = RG_OVERLAY_ALPHA_LEVEL_COUNT - 1;
    if (idx >= RG_OVERLAY_ALPHA_LEVEL_COUNT)
        idx = 0;
    rg_overlay_set_alpha(rg_overlay_alpha_levels[idx]);
}

/* ---------------------------------------------------------------- X/Y ↔ L/R 调换 */

void rg_overlay_get_swap_rect(int *x, int *y, int *w, int *h)
{
    if (x) *x = RG_TAB5_SWAP_X - RG_TAB5_SWAP_W / 2 - RG_OVERLAY_SWAP_PAD;
    if (y) *y = RG_TAB5_SWAP_Y - RG_TAB5_SWAP_H / 2 - RG_OVERLAY_SWAP_PAD;
    if (w) *w = RG_TAB5_SWAP_W + 2 * RG_OVERLAY_SWAP_PAD;
    if (h) *h = RG_TAB5_SWAP_H + 2 * RG_OVERLAY_SWAP_PAD;
}

bool rg_overlay_get_swap(void)
{
    return swap_yx;
}

/* ── 备用（调换后）那套掩码：延迟构建（见 rg_overlay_init 末尾的说明）─────────
 * 内容与"基础那套"一一对应，只是每个受影响的单元换成同一个位置的另一个键
 * （含它的标签与配色）。构建代价约占整个建层的 1/3（真机 314ms 里约 99ms），
 * 所以推到第一次真正需要时再做。只建 X/Y/L/R 四个键 + 那颗切换按钮：
 * 调换集合在这几个位置上闭合，"另一套"必然还是这套键。 */
static void rg_overlay_build_variants(void)
{
    for (size_t i = 0; i < btn_count; ++i)
    {
        const rg_key_t k = btns[i].key;
        if (k != RG_KEY_X && k != RG_KEY_Y && k != RG_KEY_L && k != RG_KEY_R)
            continue;
        const rg_key_t alt = rg_touch_swap_key(k);
        rg_overlay_btn_t tmp = btns[i];
        tmp.key = alt;
        tmp.mask = NULL;
        build_button(&tmp, overlay_key_color(alt), NULL);
        btns[i].key_alt = alt;
        btns[i].mask_alt = tmp.mask;
        memcpy(btns[i].pal_alt, tmp.pal, sizeof(tmp.pal));
        memcpy(btns[i].pal_p_alt, tmp.pal_p, sizeof(tmp.pal_p));
    }
    {
        /* 这颗按钮的颜色/调色板两套相同（恒为琥珀），变的只有标签 → 只留备用掩码 */
        rg_overlay_btn_t tmp = swap_btn;
        tmp.mask = NULL;
        build_button(&tmp, c565(0xE8, 0xA2, 0x2C), swap_yx ? "X/Y" : "L/R");
        swap_btn.mask_alt = tmp.mask;
    }
    {
        /* 备用的那一套必须真的建出来了 —— 少了它"点一下"只会改配色、标签不动。
         * 一行日志是这条链路上最便宜的体检（点一下之前就能在串口上看到）。 */
        int alt_count = 0;
        for (size_t i = 0; i < btn_count; ++i)
            if (btns[i].mask_alt)
                alt_count++;
        RG_LOGI("touch overlay: swap variants built: %d/4 key alts, switch btn alt=%d\n",
                alt_count, swap_btn.mask_alt != NULL);
    }
}

/* 第一次真正需要"另一套"时构建它（幂等，只建一次）。
 * 唯一调用点：rg_overlay_set_swap（输入线程）—— 全局只有它消费备用那套掩码
 * （rg_input.c 点那颗按钮 → set_swap）。**故意不挂到合成路径上**：挂在"第一帧合成"
 * 上只是把这 99ms 从建层挪到首帧，用户看到的开机时间一点没少；现在是首帧先出去、
 * 备用那套等玩家第一次点调换键时再建（那一下就慢约 99ms，一次性，之后都是对换指针）。
 * 也因此不需要锁：只有一个调用点。 */
static void rg_overlay_ensure_variants(void)
{
    if (!variants_pending)
        return;
    variants_pending = false;
    rg_overlay_build_variants();
}

/* 把当前生效的一套与备用的一套对换（X/Y/L/R 四个键 + 那颗切换按钮）。
 * 只做 key/掩码/调色板的对换 —— **不分配、不释放**：
 *   - 输入任务里直接调用也安全（显示线程不会读到正被释放的掩码），
 *     代价只是"正在画的那一帧可能拿到旧一套"，下一帧就对；
 *   - 不需要等下一帧重建，所以点一下画面立刻变（真机反馈"要切界面才生效"的另一半原因）。
 * 切换窗口内的调色板对换是逐项赋值，理论上存在"一帧颜色串色"的窗口 ——
 * 与模块既有的 update_acov() 同类（那也是在跑动中改共享状态），可接受。 */
static void apply_swap_variants(void)
{
    if (!ready || !btns)
        return;
    for (size_t i = 0; i < btn_count; ++i)
    {
        rg_overlay_btn_t *b = &btns[i];
        if (!b->mask_alt)               /* 不受调换影响 */
            continue;
        uint8_t *m = b->mask;       b->mask = b->mask_alt;      b->mask_alt = m;
        rg_key_t k = b->key;        b->key = b->key_alt;        b->key_alt = k;
        for (int r = 0; r < 4; ++r)
        {
            uint16_t c = b->pal[r];     b->pal[r] = b->pal_alt[r];     b->pal_alt[r] = c;
            uint16_t p = b->pal_p[r];   b->pal_p[r] = b->pal_p_alt[r]; b->pal_p_alt[r] = p;
        }
        dirty_units |= (1u << i);   /* 只有这几个单元的外观变了 —— 只重画它们 */
    }
    if (swap_btn.mask_alt)
    {
        uint8_t *m = swap_btn.mask;
        swap_btn.mask = swap_btn.mask_alt;
        swap_btn.mask_alt = m;
        dirty_swap_btn = true;
    }
    swap_generation++;
}

void rg_overlay_set_swap(bool on)
{
    if (swap_yx == on)
        return;
    swap_yx = on;
    swap_flash_ms = (uint32_t)(rg_system_timer() / 1000);   /* 点一下亮一次 */
    rg_overlay_ensure_variants();   /* 延迟构建：万一开机后第一次合成还没跑到就先点了它 */
    apply_swap_variants();     /* 立刻生效（只对换，无分配）；它自己标记"哪几个单元要重画" */
    rg_settings_set_boolean(NS_GLOBAL, SETTING_SWAP, on);
    pending_commit = true;      /* 立刻生效、延后落盘（同 set_visible） */
    /* 让画面立刻跟上：标记"哪几个单元的外观变了"，由显示线程在推帧时消费
     * （见 rg_overlay_take_dirty_rects 的说明）。⚠ 绝不要在这里直接调
     * rg_display_force_redraw() —— 它会 dispatch RG_EVENT_REDRAW，启动器的
     * event_handler 收到就 gui_redraw()，于是在**输入任务的上下文里重画整个界面**，
     * 和启动器自己的循环抢同一块 surface（真机 2026-09-29：列表里点击会短暂花屏）。 */
    RG_LOGI("touch overlay: X/Y <-> L/R swapped = %d (gen %u)\n",
            on, (unsigned)swap_generation);
}

uint32_t rg_overlay_get_generation(void)
{
    return swap_generation;
}

void rg_overlay_debug_set_pressed(uint32_t mask)
{
    debug_mask = mask;
}

/* ── "控制条带上哪些单元需要整块重画"：见文件上方 dirty_* 的声明 ── */
static void poll_pressed(void)
{
    uint32_t now = (uint32_t)(rg_system_timer() / 1000);
    uint32_t mask = rg_input_get_pressed_mask() | debug_mask;

    /* 按下状态 = 真实按下 + 短暂保持（快速点按也能看到一次反馈） */
    visual_mask = mask;
    for (size_t i = 0; i < btn_count; ++i)
    {
        const rg_key_t k = btns[i].key;
        if (mask & k)
        {
            last_press_ms[i] = now;
            continue;
        }
        /* 刚松手的一小段时间内保持高亮（快速点按也看得见）。
         * last_press_ms[i] == 0 时 now-0 是大数 → 自然不命中，不需要额外初值处理。 */
        if ((uint32_t)(now - last_press_ms[i]) < RG_OVERLAY_PRESS_LINGER_MS)
            visual_mask |= k;
    }

    /* 高亮位变了（按下/松手/linger 结束）→ 记下"哪些单元需要整块重画"。
     * 谁用：显示层推帧循环到不了控制条带的场合（游戏里），见 rg_overlay_take_dirty_rects。 */
    if (visual_mask != dirty_last_visual)
    {
        const uint32_t changed = visual_mask ^ dirty_last_visual;
        for (size_t i = 0; i < btn_count; ++i)
            if (changed & btns[i].key)
                dirty_units |= (1u << i);
        dirty_last_visual = visual_mask;
    }

    /* ── 触摸抖动观测（只统计，不改行为；先量再决定要不要加滤波）───────────────
     * 判据：手指稳定按住时高亮位只应有"按下 / 松手"两次跳变；若在 30ms 内反复跳，
     * 就是命中抖动（手指压在两个键的边界上，判定来回切）。正常点按因为 linger
     * 机制至少隔 RG_OVERLAY_PRESS_LINGER_MS，不会误报。
     * 只在"连续多次快跳"时打一行 DEBUG，避免刷屏；burst 每到 5 就清一次，
     * 所以持续抖动会每 5 次跳变留一行，正好能数出抖动频率。 */
    if (visual_mask != dirty_last_visual_before)
    {
        static uint32_t jitter_last_ms = 0;
        static int jitter_burst = 0;
        const uint32_t gap = now - jitter_last_ms;
        if (jitter_last_ms != 0 && gap < 30)
        {
            if (++jitter_burst >= 5)
            {
                RG_LOGD("touch: jitter burst (%d transitions, last gap %u ms)\n",
                        jitter_burst, (unsigned)gap);
                jitter_burst = 0;
            }
        }
        else if (jitter_burst)
            jitter_burst = 0;
        jitter_last_ms = now ? now : 1;
    }
    dirty_last_visual_before = visual_mask;
}

int rg_overlay_take_dirty_rects(int *out_xywh, int max)
{
    rg_overlay_try_panel();     /* 第一帧真正要画时把皮肤面板装上（见上面的时序说明） */
    if (!out_xywh || max <= 0)
        return 0;

    /* 面板底/凹槽/铭牌整条变过：直接把"整条控制区"当一个矩形交出去（显示层会用 border
     * 重铺这块，覆盖层由驱动合成），比逐键返回 13 个矩形更省也更彻底。 */
    if (dirty_panel_strip)
    {
        dirty_panel_strip = false;
        dirty_units = 0;
        dirty_swap_btn = false;
        dirty_all_units = false;
        out_xywh[0] = 0;
        out_xywh[1] = ctrl_top_cur;                       /* 跟视口走（GBA 480 / GB 456 / NES 480） */
        out_xywh[2] = RG_SCREEN_WIDTH;
        /* ⚠ 必须是**画布高度**（横屏 720 / 竖屏 1280），不能写死 1280：
         * 2026-10-08 夜真机日志 —— 横屏下写死 1280 得到 (0,480,1280,800)，
         * 显示层再按"画布→可见区"归一后越界 → `lcd_set_window: Bad lcd window
         * (x0=280, y0=600, w=1280, h=240)`（同一条日志里还跟着一次 msync 报错）。
         * 竖屏下 RG_SCREEN_HEIGHT 就是 1280，故竖屏行为逐字节不变。 */
        out_xywh[3] = RG_SCREEN_HEIGHT - ctrl_top_cur;
        return 1;
    }

    int n = 0;
    for (size_t i = 0; i < btn_count && n < max; ++i)
    {
        if (!(dirty_units & (1u << i)) && !dirty_all_units)
            continue;
        out_xywh[n * 4 + 0] = btns[i].x;
        out_xywh[n * 4 + 1] = btns[i].y;
        out_xywh[n * 4 + 2] = btns[i].w;
        out_xywh[n * 4 + 3] = btns[i].h;
        n++;
    }
    if (swap_btn_ready && n < max && (dirty_swap_btn || dirty_all_units))
    {
        out_xywh[n * 4 + 0] = swap_btn.x;
        out_xywh[n * 4 + 1] = swap_btn.y;
        out_xywh[n * 4 + 2] = swap_btn.w;
        out_xywh[n * 4 + 3] = swap_btn.h;
        n++;
    }

    dirty_units = 0;
    dirty_swap_btn = false;
    dirty_all_units = false;
    return n;
}

/* ---------------------------------------------------------------- 合成 */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* RGB565 定点混合：dst = src*a + dst*(1-a)，a 为 0..64 的 6 位权重。
 * 用通道掩码乘法（0xF81F 隔开 R/B，0x07E0 是 G），一次算两个通道。
 *
 * ⚠ 权重必须 ≤ 6 位、右移 6 位：蓝通道乘权重后最多占 11 位（0..10），红通道从第 11 位起 ——
 *   7 位以上权重会让蓝的进位撞进红字段。踩过：8 位权重 + 掩码 0xF81F + >>8 这个网上流传的
 *   写法在蓝通道最大偏差 24/31（肉眼可见的偏色，实测 A 键红色边框发紫），6 位权重误差 ≤1。 */
static inline void blend_px(uint16_t *dst, uint16_t src, int a)
{
    if (a <= 0)
        return;
    if (a >= AW_ONE)
    {
        *dst = src;
        return;
    }
    uint32_t d = *dst, s = src;
    uint32_t na = (uint32_t)(AW_ONE - a);
    uint32_t rb = (((s & 0xF81Fu) * (uint32_t)a) + ((d & 0xF81Fu) * na)) >> AW_BITS;
    uint32_t g  = (((s & 0x07E0u) * (uint32_t)a) + ((d & 0x07E0u) * na)) >> AW_BITS;
    *dst = (uint16_t)((rb & 0xF81Fu) | (g & 0x07E0u));
}

/* 单个按键的合成（两种朝向共用一份）。cw90=1 时按 tab5 物理朝向换算索引：
 * 逻辑 (lx,ly) -> 物理 (px,py) = (phys_w-1-ly, lx)。早退条件由调用方保证。 */
static void blit_one(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w,
                     const rg_overlay_btn_t *b, bool cw90, bool force_press)
{
    if (!b->mask)
        return;   /* init 时分配失败的按键：跳过，别解引用空指针 */

    int bx0, by0, bw, bh;
    if (cw90)
    {
        bx0 = phys_w - b->y - b->h; by0 = b->x; bw = b->h; bh = b->w;
    }
    else
    {
        bx0 = b->x; by0 = b->y; bw = b->w; bh = b->h;
    }

    int ix0 = imax(bx0, rx), ix1 = imin(bx0 + bw, rx + rw);
    int iy0 = imax(by0, ry), iy1 = imin(by0 + bh, ry + rh);
    if (ix0 >= ix1 || iy0 >= iy1)
        return;

    /* force_press：给"没有游戏键位"的自绘按钮用（L/R 之间那颗调换按钮 key=0，
     * 靠 visual_mask 永远匹配不上，按下反馈由调用方传进来）。 */
    const bool pr = force_press || (b->key && (visual_mask & b->key) != 0);
    const uint8_t (*ac)[17] = pr ? acov_p : acov;
    const uint16_t *pal = pr ? b->pal_p : b->pal;

    for (int py = iy0; py < iy1; ++py)
    {
        uint16_t *dst = buf + (size_t)(py - ry) * stride + (ix0 - rx);
        const int lx_off = py - by0;                  /* = lx - b->x */
        for (int px = ix0; px < ix1; ++px)
        {
            uint8_t v;
            if (cw90)
            {
                const int ly_off = b->h - 1 - (px - bx0);   /* 物理 x 反向对应逻辑 y */
                v = b->mask[(size_t)ly_off * b->w + lx_off];
            }
            else
            {
                v = b->mask[(size_t)lx_off * b->w + (px - bx0)];
            }
            if (v)
                blend_px(&dst[px - ix0], pal[v & 7], ac[v & 7][v >> 3]);
        }
    }
}

/* 命中矩形 = 视觉矩形外扩 RG_OVERLAY_TOGGLE_PAD（见文件头宏处的说明）。
 * 视觉矩形仍由 RG_OVERLAY_TOGGLE_{X,Y,W,H} 决定（blit_toggle 用）。 */
void rg_overlay_get_toggle_rect(int *x, int *y, int *w, int *h)
{
    if (x) *x = RG_OVERLAY_TOGGLE_X - RG_OVERLAY_TOGGLE_PAD;
    if (y) *y = RG_OVERLAY_TOGGLE_Y - RG_OVERLAY_TOGGLE_PAD;
    if (w) *w = RG_OVERLAY_TOGGLE_W + 2 * RG_OVERLAY_TOGGLE_PAD;
    if (h) *h = RG_OVERLAY_TOGGLE_H + 2 * RG_OVERLAY_TOGGLE_PAD;
}

/* 设置改了以后延后到这里落盘。为什么不在 set_visible 里直接写：
 * 那个 setter 可能被输入任务调用（点开关），而写 SD 卡不能跨任务并发
 * —— 合成路径跑在 GUI/模拟器主任务上，和菜单改设置是同一个上下文，安全。 */
static void commit_if_pending(void)
{
    if (!pending_commit)
        return;
    pending_commit = false;
    rg_settings_commit();
}

/* 建层失败后的自愈：每 1 秒重试一次（PSRAM 分配失败常常是暂时的）。
 * 重试期间 ready=false → 按键不画、输入也被 is_ready() 一起关掉（同源）。 */
static void retry_init_if_needed(void)
{
    if (ready)
        return;
    static int64_t next_retry = 0;
    const int64_t now = rg_system_timer();
    if (now < next_retry)
        return;
    next_retry = now + 1000000;
    RG_LOGW("touch overlay: not ready, retrying init\n");
    rg_overlay_init();
}

/* 左上角"把按键放回来"的开关。**不走掩码，直接按几何逐像素画**：
 *  ① 88x48 很小，每帧重画的代价可忽略；
 *  ② 掩码分配失败（ready=false）时它也必须画得出来 —— 它是隐藏态下唯一的入口，
 *     "画不出来"等于用户被困在无输入状态。
 * 白色系：和按键的彩色区分开，它是"工具"不是按键。 */
static void blit_toggle(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w, bool cw90)
{
    const int tw = RG_OVERLAY_TOGGLE_W, th = RG_OVERLAY_TOGGLE_H;
    int bx0, by0, bw, bh;
    if (cw90) { bx0 = phys_w - RG_OVERLAY_TOGGLE_Y - th; by0 = RG_OVERLAY_TOGGLE_X; bw = th; bh = tw; }
    else      { bx0 = RG_OVERLAY_TOGGLE_X;             by0 = RG_OVERLAY_TOGGLE_Y; bw = tw; bh = th; }

    const int ix0 = imax(bx0, rx), ix1 = imin(bx0 + bw, rx + rw);
    const int iy0 = imax(by0, ry), iy1 = imin(by0 + bh, ry + rh);
    if (ix0 >= ix1 || iy0 >= iy1)
        return;

    const int m = (tw < th ? tw : th);
    const int arm = m * RG_OVERLAY_TOGGLE_ARM_PCT / 100;
    const int wid = m * RG_OVERLAY_TOGGLE_WID_PCT / 100;
    const int cx = tw / 2, cy = th / 2;
    const int a_line = alpha_level * 64 / 255;      /* 边框/十字：跟随透明度档位 */
    const int a_fill = a_line * 35 / 100;           /* 填充：更暗，让白十字跳出来 */

    for (int py = iy0; py < iy1; ++py)
    {
        uint16_t *dst = buf + (size_t)(py - ry) * stride + (ix0 - rx);
        for (int px = ix0; px < ix1; ++px)
        {
            int lx, ly;
            if (cw90) { lx = py - by0; ly = tw - 1 - (px - bx0); }
            else      { lx = px - bx0; ly = py - by0; }

            const bool border = (lx < RG_OVERLAY_TOGGLE_BORDER || lx >= tw - RG_OVERLAY_TOGGLE_BORDER ||
                                 ly < RG_OVERLAY_TOGGLE_BORDER || ly >= th - RG_OVERLAY_TOGGLE_BORDER);
            const int dx = lx - cx, dy = ly - cy;
            const bool cross = !border &&
                (((dx >= -wid && dx <= wid) && (dy >= -arm && dy <= arm)) ||
                 ((dy >= -wid && dy <= wid) && (dx >= -arm && dx <= arm)));
            if (border || cross)
                blend_px(&dst[px - ix0], 0xFFFF, a_line);
            else
                blend_px(&dst[px - ix0], c565_scale(0xFFFF, 30, 100), a_fill);
        }
    }
}

/* L/R 之间那颗「X/Y ↔ L/R 调换」按钮的合成。
 * 它不是游戏键（key=0），按下反馈不来自 visual_mask 而是来自"刚点过"的时间戳
 * （点一下亮一次 = 告诉用户"点到了，换过来了"）。隐藏态不画：整排按键都没了，
 * 单独留一颗 UI 按钮反而奇怪（恢复入口是左上角那颗开关）。 */
static void blit_swap_btn(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w,
                          bool cw90)
{
    if (!swap_btn_ready)
        return;
    const uint32_t now = (uint32_t)(rg_system_timer() / 1000);
    const bool flash = (uint32_t)(now - swap_flash_ms) < RG_OVERLAY_PRESS_LINGER_MS;
    /* 闪一下是这颗按钮自己的外观变化（它不是游戏键，不会进 visual_mask）：
     * 记下闪的起止，让显示层把条带上的它重画一遍（游戏里推帧到不了条带）。 */
    static bool flash_last = false;
    if (flash != flash_last)
    {
        dirty_swap_btn = true;
        flash_last = flash;
    }
    blit_one(buf, stride, rx, ry, rw, rh, phys_w, &swap_btn, cw90, flash);
}

void rg_overlay_blit(uint16_t *buf, int stride, int rx, int ry, int rw, int rh)
{
    rg_overlay_try_panel();
    if (!buf || rw <= 0 || rh <= 0)
        return;

    retry_init_if_needed();
    commit_if_pending();
    poll_pressed();

    if (!visible || !ready)
    {
        /* 隐藏态（或建层失败）：只画左上角开关 —— 它是唯一入口，任何情况下都得在 */
        blit_toggle(buf, stride, rx, ry, rw, rh, 0, false);
        return;
    }

    for (size_t i = 0; i < btn_count; ++i)
        blit_one(buf, stride, rx, ry, rw, rh, 0, &btns[i], false, false);

    blit_swap_btn(buf, stride, rx, ry, rw, rh, 0, false);
}

/* ---------------------------------------------------------------- 屏幕上的显示帧率数字
 *
 * 位置：L 与 R 肩键之间的顶部中央（逻辑 (640,60)，与肩键同一行；两键分别占 x 40~240 与
 *       1040~1240，中间这段本来就是空的，不压游戏画面）。
 * 数据来源：rg_system.c 的 update_statistics() 每秒推一次值（partialFPS + fullFPS，
 *           "真正显示出去的帧率"，与日志 FPS:(跳过+部分+完整) 的后两项同口径）。
 * 实现：复用本模块已加载的 8x8 点阵**逐像素直绘**，不走按键掩码 —— 数字每秒都在变，
 *       为它反复重建掩码没意义；面积仅 3 位 × scale4 = 96×32 = 3072 px，代价可忽略。
 * 颜色：纯白 + 黑色投影（先投影、后正文），任何游戏画面上都看得清。 */
#define RG_FPS_TEXT_CX 360      /* 逻辑坐标：控制区顶部正中（L(50~230) 与 R(490~670) 肩键之间的空白带）*/
#define RG_FPS_TEXT_CY 500      /* ⚠ 竖屏口径：y<480 是游戏画面，数字只能落在控制区（走查 P2-11） */
#define RG_FPS_SCALE   4

static int fps_value = -1;
static char fps_text[8] = "";

void rg_overlay_set_fps(int value)
{
#if !RG_OVERLAY_SHOW_FPS
    /* 发布版：不显示帧率数字 */
    (void)value;
    return;
#endif

    if (value < 0)
    {
        fps_value = -1;
        return;
    }
    if (value > 999)
        value = 999;
    if (value == fps_value)
        return;                       /* 值没变就不必重排版 */
    fps_value = value;
    snprintf(fps_text, sizeof(fps_text), "%d", value);
}

/* 按 cw90 映射把 fps_text 直绘进 buf：逻辑 (lx,ly) -> 物理 (phys_w-1-ly, lx)。
 * 与 blit_one 用同一套映射；这里逐像素换算，不用第二份旋转位图。 */
static void draw_fps_text(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w,
                          int lx0, int ly0, const uint16_t color)
{
    const int n = (int)strlen(fps_text);
    (void)phys_w;   /* 竖屏线性映射下不再需要物理宽度换算（见下） */
    /* ⚠ 竖屏线性映射：逻辑 (lx,ly) == 物理 (px,py)。
     * 老代码这里是 cw90 的反向换算（px = phys_w - ly0 - ...）—— 那是横屏时代的写法，
     * 竖屏下启用帧率数字会把它画到完全错误的位置（走查 P2-11，已按线性改写）。 */
    const int px0 = lx0;
    const int px1 = lx0 + n * 8 * RG_FPS_SCALE;
    const int py0 = ly0;
    const int py1 = ly0 + 8 * RG_FPS_SCALE;

    const int cx0 = imax(px0, rx), cx1 = imin(px1, rx + rw);
    const int cy0 = imax(py0, ry), cy1 = imin(py1, ry + rh);
    if (cx0 >= cx1 || cy0 >= cy1)
        return;                                        /* 本块不覆盖数字区域，早退 */

    for (int py = cy0; py < cy1; ++py)
    {
        const int gy = (py - py0) / RG_FPS_SCALE;      /* 逻辑 y -> 字形行 */
        if (gy < 0 || gy > 7)
            continue;
        for (int px = cx0; px < cx1; ++px)
        {
            const int gx = (px - px0) / RG_FPS_SCALE;  /* 逻辑 x -> 字形列 */
            const int ch = gx >> 3, col = gx & 7;
            if (ch >= n)
                continue;
            const uint8_t *gl = glyphs[(uint8_t)fps_text[ch]];
            if (gl[gy] & (0x80 >> col))                /* 字模 MSB = 最左列 */
                buf[(size_t)(py - ry) * stride + (px - rx)] = color;
        }
    }
}

static void blit_fps(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w)
{
#if !RG_OVERLAY_SHOW_FPS
    /* 发布版：不显示帧率数字 */
    return;
#endif

    if (fps_value < 0 || !fps_text[0])
        return;
    const int n = (int)strlen(fps_text);
    const int lx = RG_FPS_TEXT_CX - n * 8 * RG_FPS_SCALE / 2;
    const int ly = RG_FPS_TEXT_CY - 8 * RG_FPS_SCALE / 2;
    draw_fps_text(buf, stride, rx, ry, rw, rh, phys_w, lx + 2, ly + 2, c565(0, 0, 0));      /* 投影 */
    draw_fps_text(buf, stride, rx, ry, rw, rh, phys_w, lx, ly, c565(255, 255, 255));        /* 正文 */
}

/* ---------------------------------------------------------------- 电量圆灯
 *
 * 位置：控制区**上下两排按键之间的空档正中** —— 十字/ABXY 底边 907、系统键顶边 1142，
 *       取 (907+1142)/2 ≈ 1025。x=360 是屏幕中线，也正好是 START / 十字 / ABXY 的对称轴。
 *       （2026-09-29 用户第一版反馈："不居中" → 原来贴在 START 上方显得偏下，已上移到空档中心。）
 * 样式：**按键同款配方**（用户反馈："其他按键都是加个框的"）——
 *       外圈 = 键色 @ α*0.90（与按键边框同配方）、内芯 = 键色*0.55 @ α*0.50、圈宽 3px；
 *       颜色不新造，直接复用调色板里已有的三个色（见 batt_led_color 注释）。
 * 闪烁：只灭内芯、外圈压暗保留（26%）—— 像一盏没点亮的指示灯，不会整块凭空消失又冒出来。
 * 亮度：跟随叠加层透明度档位（α 调到 20% 时灯一起暗下去，整屏一致）。
 * 颜色：绿 100~60% / 橙 60~20% / 红 20~10% / <10% 红闪（**硬闪**，告警要抓眼）。
 * 充电中 → 绿**呼吸**（优先级最高；4 档亮度 255/196/148/196、每档 340ms，整周期 ≈1.4s）。
 *   v0.4.1 起改的：原来充电也是硬闪，但 INA226 分流采样会抖，叠加硬闪显得不自然。
 * 相位：由 rg_system_timer 决定，无状态、免定时器。
 *   · 低电告警：500ms 亮 / 500ms 暗（硬闪）
 *   · 充电呼吸：走 bright 亮度系数（见 batt_led_state 的说明）
 * 背景：控制区这块没有任何内容 → 直写时先擦黑再画（不擦会新旧叠加，同帧率数字的教训）。
 * 边缘：2x2 子采样求覆盖率（外圈/内芯各一次），背景是黑 → 按覆盖率压暗即向背景混合。
 *
 * ⚠ 行号必须是 4 的倍数：帧缓冲一行 = 720px×2B = 1440B，1440 % 128 = 32，
 *   只有每 4 行才落到 128B 边界上 —— cache 写回（esp_cache_msync）要求 128B 对齐。
 *   圆占 y 1013..1037，取 [1012,1040) 正好满足。 */
/* （几何 RG_BATT_LED_* 已提到文件头部 —— 皮肤面板的铭牌锚点要用） */
#ifndef RG_SCREEN_WIDTH
#define RG_SCREEN_WIDTH 720
#endif

/* 键色取自现成调色板（保证整屏同一语言）：绿 #4CB05A = Y 键绿、橙 #E8A22C = MENU 琥珀、
 * 红 #E24B3F = A 键红。颜色只表示电量档位，不承担别的语义。 */
static uint16_t batt_led_color(int idx)
{
    switch (idx)
    {
        case 1:  return c565(0x4C, 0xB0, 0x5A);
        case 2:  return c565(0xE8, 0xA2, 0x2C);
        case 3:  return c565(0xE2, 0x4B, 0x3F);
        default: return c565(0x4C, 0xB0, 0x5A);
    }
}

/* 返回要保留的色相（1 绿 / 2 橙 / 3 红；0 = 无电池，不画）。
 * *lit = 这一相位亮不亮（硬闪，仅用于低电告警）；*bright = 0~255 亮度系数
 * （充电态用它做**柔和呼吸**：4 档/340ms → 整周期 ≈1.4s，最暗只降到 148/255。
 *  用户 2026-09-29 反馈 1Hz 硬闪"闪了但不是很自然"，故充电改为呼吸、告警保留硬闪）。 */
static int batt_led_state(bool *lit, int *bright)
{
    const rg_battery_t b = rg_input_read_battery();

    *lit = true;
    *bright = 255;
    if (!b.present)
        return 0;                              /* 没装电池 / 读不到 → 不显示（不是红色告警） */

    int idx;
    bool blink = false;

    if (b.charging)
    {
        static const int breath[4] = {255, 196, 148, 196};   /* 亮→暗→亮，来回是正弦的味道 */
        *bright = breath[(int)((rg_system_timer() / 340000) & 3)];
        return 1;                              /* 充电中：绿 + 呼吸（压过一切电量颜色） */
    }
    else if (b.level < 10.f) { idx = 3; blink = true; }   /* <10%：红闪（故意保持硬闪，告警要抓眼） */
    else if (b.level < 20.f) { idx = 3; }
    else if (b.level < 60.f) { idx = 2; }
    else                     { idx = 1; }

    if (blink && ((rg_system_timer() / 500000) & 1))
        *lit = false;                          /* 500ms 暗相位（外圈保留、内芯灭） */
    return idx;
}

bool rg_batt_led_refresh_needed(void)
{
    static int last = -1;
    bool lit = true;
    int bright = 255;
    /* 色相 + 亮灭 + 呼吸档位一起编码（档位取 >>5 得 8 级，够区分我们的 4 档） */
    const int code = batt_led_state(&lit, &bright) * 8192 + (lit ? 4096 : 0) + (bright >> 5);
    if (code == last)
        return false;
    last = code;
    return true;
}

void rg_batt_led_get_band(int *x0, int *y0, int *x1, int *y1)
{
    /* 条带跟着圆灯**实际位置**走（灯位由皮肤布局推导 → led_cy_cur），不再写死 1012/1040。
     * 行号按 4 取整：帧缓冲一行 1440B，128B 边界每 4 行才落一次，msync 要求偏移/长度都对齐。 */
    const int lo = led_cy_cur - RG_BATT_LED_R - RG_BATT_LED_BAND_PAD;
    const int hi = led_cy_cur + RG_BATT_LED_R + RG_BATT_LED_BAND_PAD;
    if (x0) *x0 = 0;
    if (y0) *y0 = (lo & ~3) < 0 ? 0 : (lo & ~3);
    if (x1) *x1 = RG_SCREEN_WIDTH;
    if (y1) *y1 = ((hi + 3) & ~3) > RG_SCREEN_HEIGHT ? RG_SCREEN_HEIGHT : ((hi + 3) & ~3);
}

/* 擦灯条带用的背景色 = **当前皮肤的面板底色**（已量化 565）。
 * 面板还没装（启动器/横屏阶段）时退回纯黑，与那时的黑底一致。 */
uint16_t rg_batt_led_band_bg(void)
{
    if (!panel_installed)
        return 0x0000;
    return rg_touch_skin_panel_bg565(skin_idx);
}

/* 圆灯像素算法（**唯一一份**）：竖屏线性写、横屏 90CW 映射写，只有索引不同。
 *   cw90=false：物理 = 逻辑，写 buf[ly*stride + lx]（竖屏驱动用，字节与改前完全一致）
 *   cw90=true ：物理 px = phys_w-1-ly、py = lx（横屏驱动用）——
 *               口径必须与 rg_overlay_blit_cw90() / mipi_dsi_tab5.h 的映射完全一致。
 * 横屏不套"灯条带"裁剪（那是竖屏整行擦写用的），只受画布边界约束：
 * 灯本身就是一个小方块，物理侧由调用方按 rg_batt_led_get_rect()+映射算出矩形来擦/写回。 */
static void batt_led_draw_impl(uint16_t *buf, int stride, bool cw90, int phys_w)
{
    if (!buf || stride <= 0)
        return;
    /* 契约（走查 P2-15）：buf 是**整屏**帧缓冲（竖屏线性映射下逻辑与物理同向），
     * stride = 一行像素数。本函数只写「灯条带」内的像素（条带范围由 rg_batt_led_get_band()
     * 给出，跟着 led_cy_cur 走），且是**直接覆盖**。
     * 调用方（显示驱动 tab5_batt_led_refresh）负责：① 先把条带擦成背景
     * （背景色 = rg_batt_led_band_bg()，即当前皮肤的面板底色 —— 用纯黑会在非黑面板上割裂画面）
     * ② 画完后对条带做 C2M cache 写回（CPU 写、DMA 读，方向不能反）。 */

    bool lit = true;
    int bright = 255;
    const int idx = batt_led_state(&lit, &bright);
    if (!idx)
        return;                                /* 无电池：条带已被驱动擦成背景，这里什么都不画 */

    const uint16_t base = batt_led_color(idx);
    /* 与按键同配方（背景是黑，所以按系数压暗 = 向背景混合，不必走 blend_px）：
     * 外圈 键色*100%（亮）/ 26%（暗） @ α*0.90（暗相位 α*0.72）；内芯 键色*55% / 6% @ α*0.50 */
    const int ring_pct = lit ? 100 : 26;
    const int fill_pct = lit ? 55 : 6;
    const int a_ring = alpha_level * (lit ? 90 : 72) / 100;
    const int a_fill = alpha_level * 50 / 100;
    const int R = RG_BATT_LED_R, Ri = R - RG_BATT_LED_RING;
    int bx0, by0, bx1, by1;
    rg_batt_led_get_band(&bx0, &by0, &bx1, &by1);

    for (int dy = -R; dy <= R; ++dy)
    {
        const int ly = led_cy_cur + dy;
        uint16_t *row = NULL;                 /* 线性路径的整行基址；cw90 路径不用它 */
        if (!cw90)
        {
            if (ly < by0 || ly >= by1)
                continue;
            row = buf + (size_t)ly * stride;
        }
        else if (ly < 0 || ly >= RG_SCREEN_HEIGHT)
        {
            continue;                          /* cw90 下 ly 决定**物理列**，受逻辑高约束 */
        }
        for (int dx = -R; dx <= R; ++dx)
        {
            const int lx = RG_BATT_LED_CX + dx;
            if (lx < 0 || lx >= RG_SCREEN_WIDTH)
                continue;
            int cov_out = 0, cov_in = 0;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx)
                {
                    const float px = dx + (sx ? 0.25f : -0.25f);
                    const float py = dy + (sy ? 0.25f : -0.25f);
                    const float d2 = px * px + py * py;
                    if (d2 <= (float)(R * R))
                        cov_out++;
                    if (d2 <= (float)(Ri * Ri))
                        cov_in++;
                }
            if (!cov_out)
                continue;
            /* 圈与芯同源（芯 = 键色*0.55）→ 合成一次缩放：分子 = Σ α*覆盖率*色深%
             * 再乘充电呼吸的亮度系数 bright/255（硬闪时 bright=255，行为与以前一致）。 */
            const int num = (a_ring * (cov_out - cov_in) * ring_pct + a_fill * cov_in * fill_pct) * bright / 255;
            if (cw90)
                buf[(size_t)lx * stride + (phys_w - 1 - ly)] = c565_scale(base, num, 4 * 255 * 100);
            else
                row[lx] = c565_scale(base, num, 4 * 255 * 100);
        }
    }
}

/* 竖屏入口：线性写（逻辑 = 物理）。签名不变 —— 竖屏驱动那一处调用不用动。 */
void rg_batt_led_draw(uint16_t *buf, int stride)
{
    batt_led_draw_impl(buf, stride, false, 0);
}

/* 横屏入口：90CW 映射写。phys_w = 物理面板宽（= 逻辑屏高，720），一路传给索引换算。 */
void rg_batt_led_draw_cw90(uint16_t *buf, int stride, int phys_w)
{
    batt_led_draw_impl(buf, stride, true, phys_w);
}

/* 灯的**紧贴逻辑矩形**（含 BAND_PAD 余量）：横屏驱动用它按映射算出物理矩形，
 * 再做"擦底 + 定点 msync"（竖屏不用它 —— 竖屏擦的是跟随灯位的整行条带，见 rg_batt_led_get_band）。 */
void rg_batt_led_get_rect(int *x0, int *y0, int *x1, int *y1)
{
    const int p = RG_BATT_LED_R + RG_BATT_LED_BAND_PAD;
    if (x0) *x0 = RG_BATT_LED_CX - p;
    if (y0) *y0 = led_cy_cur - p;
    if (x1) *x1 = RG_BATT_LED_CX + p + 1;
    if (y1) *y1 = led_cy_cur + p + 1;
}

/* tab5 专用（**横屏版驱动专用**；竖屏线性分支不再调用它 —— 那份走下面的
 * rg_overlay_blit_linear()）：
 * 逻辑画面按 90°CW 写进物理帧缓冲，逻辑 (lx,ly) -> 物理 (px,py) = (phys_w-1-ly, lx)。
 * 与横屏驱动 mipi_dsi_tab5.h 的映射必须完全一致。
 * 这里不做第二份旋转位图，只在索引上换算 —— 一份数据、两种朝向。 */
void rg_overlay_blit_cw90(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w)
{
    rg_overlay_try_panel();
    if (!buf || rw <= 0 || rh <= 0)
        return;

    retry_init_if_needed();
    commit_if_pending();
    poll_pressed();

    if (!visible || !ready)
    {
        /* 隐藏态（或建层失败）：只画左上角开关 —— 它是唯一入口，任何情况下都得在 */
        blit_toggle(buf, stride, rx, ry, rw, rh, phys_w, true);
        blit_fps(buf, stride, rx, ry, rw, rh, phys_w);   /* 帧率数字与按键显隐无关，始终画 */
        return;
    }

    for (size_t i = 0; i < btn_count; ++i)
        blit_one(buf, stride, rx, ry, rw, rh, phys_w, &btns[i], true, false);

    blit_swap_btn(buf, stride, rx, ry, rw, rh, phys_w, true);
    blit_fps(buf, stride, rx, ry, rw, rh, phys_w);
}


/* 竖屏线性版（tab5p 分支）：逻辑坐标 = 物理坐标，不做 90° 换算。
 * 与 cw90 版共用同一份绘制代码，只是 cw90=false。 */
void rg_overlay_blit_linear(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w)
{
    rg_overlay_try_panel();
    if (!buf || rw <= 0 || rh <= 0)
        return;

    retry_init_if_needed();
    commit_if_pending();
    poll_pressed();

    if (!visible || !ready)
    {
        blit_toggle(buf, stride, rx, ry, rw, rh, phys_w, false);
        blit_fps(buf, stride, rx, ry, rw, rh, phys_w);
        return;
    }

    for (size_t i = 0; i < btn_count; ++i)
        blit_one(buf, stride, rx, ry, rw, rh, phys_w, &btns[i], false, false);

    blit_swap_btn(buf, stride, rx, ry, rw, rh, phys_w, false);
    blit_fps(buf, stride, rx, ry, rw, rh, phys_w);
}

#endif /* RG_GAMEPAD_TOUCH_MAP && RG_TOUCH_OVERLAY */
