/* 虚拟按键可视层实现 —— 设计说明见 rg_touch_overlay.h */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rg_touch_overlay.h"

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

#define ROLE_NONE   0
#define ROLE_BORDER 1
#define ROLE_FILL   2
#define ROLE_LABEL  3

/* NVS 键（与 rg_gui.c 菜单共用同一份定义，见 rg_touch_overlay.h） */
#define SETTING_VISIBLE RG_TOUCH_SETTING_VISIBLE
#define SETTING_ALPHA   RG_TOUCH_SETTING_ALPHA

const int rg_overlay_alpha_levels[RG_OVERLAY_ALPHA_LEVEL_COUNT] = {100, 80, 60, 40, 20};

typedef struct
{
    rg_key_t key;
    bool is_toggle;      /* true = 左上角那个"把按键放回来"的开关（不是按键，没有 key） */
    int x, y, w, h;      /* 逻辑坐标：左上角 + 尺寸 */
    uint8_t *mask;       /* w*h： (覆盖率<<3) | 角色 */
    uint16_t pal[4];     /* 正常态：角色 -> 颜色 */
    uint16_t pal_p[4];   /* 按下态 */
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

static rg_overlay_btn_t *btns;
static size_t btn_count;
static bool ready;
static bool visible = true;
static int alpha_pct = 100;
static int alpha_level = 255;             /* 0..255，= alpha_pct * 255 / 100 */
static bool pending_commit = false;       /* 设置改了但还没落盘（见 commit_if_pending） */
static uint8_t acov[4][17];               /* 正常态混合权重 [角色][覆盖率] */
static uint8_t acov_p[4][17];             /* 按下态 */
/* ⚠ 按下保持计时必须**按按钮序号**存，不能按 rg_key_t 的值存：
 * RG_KEY_* 是位掩码（RG_KEY_R = 1<<13 = 8192），拿它当下标越界 32KB —— 实机第一帧合成就
 * LoadProhibited 崩溃；宿主 .bss 邻页可读，所以 SDL2 预览完全看不出来（教训见经验沉淀 §92）。 */
static uint32_t last_press_ms[RG_OVERLAY_MAX_BUTTONS];
static uint32_t visual_mask;              /* 含"高亮保持"的显示用按下掩码 */
static uint32_t debug_mask;               /* 预览用强制按下 */
static uint8_t glyphs[128][8];            /* ASCII 8x8 点阵，MSB = 最左列 */

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
    switch (key)
    {
        case RG_KEY_UP:     /* #7C8CA6 冷灰蓝 */
        case RG_KEY_DOWN:
        case RG_KEY_LEFT:
        case RG_KEY_RIGHT:  return c565(0x7C, 0x8C, 0xA6);
        case RG_KEY_A:      return c565(0xE2, 0x4B, 0x3F);  /* #E24B3F 红 */
        case RG_KEY_B:      return c565(0xE8, 0xC3, 0x3A);  /* #E8C33A 黄 */
        case RG_KEY_X:      return c565(0x3F, 0x7A, 0xD8);  /* #3F7AD8 蓝 */
        case RG_KEY_Y:      return c565(0x4C, 0xB0, 0x5A);  /* #4CB05A 绿 */
        case RG_KEY_L:      /* #A8B2C0 浅灰 */
        case RG_KEY_R:      return c565(0xA8, 0xB2, 0xC0);
        case RG_KEY_SELECT: /* #6C7686 中灰 */
        case RG_KEY_START:  return c565(0x6C, 0x76, 0x86);
        case RG_KEY_MENU:   return c565(0xE8, 0xA2, 0x2C);  /* #E8A22C 琥珀 */
        default:            return c565(0xC0, 0xC0, 0xC0);
    }
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
        size_t nbytes = g->width ? ((((size_t)g->width * g->height) - 1) / 8) + 1 : 0;
        if (g->code < 128 && g->width == 8 && g->height == 8)
            for (int y = 0; y < 8; ++y)
                glyphs[g->code][y] = (uint8_t)(g->data[y] >> g->xOffset);  /* xOffset: 右移列 */
        p += sizeof(rg_font_glyph_t) + nbytes;
    }
}

/* ---------------------------------------------------------------- 标签几何 */

static void label_geom(int w, int h, rg_key_t key, bool is_toggle, rg_overlay_label_t *L)
{
    memset(L, 0, sizeof(*L));

    (void)is_toggle;   /* 开关已改为程序化绘制（见 blit_toggle），这里不再需要 */

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

    if (!text)
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

static void build_button(rg_overlay_btn_t *b, uint16_t color)
{
    const int w = b->w, h = b->h;
    rg_overlay_label_t L;
    label_geom(w, h, b->key, b->is_toggle, &L);

    float r = (float)((w < h ? w : h) * 18 / 100);
    if (r < 2) r = 2;
    float bw = BORDER_PX;
    if (bw * 2 > (w < h ? w : h) / 2) bw = (w < h ? w : h) / 4.0f;

    b->mask = rg_alloc((size_t)w * h, MEM_SLOW);
    if (!b->mask)
    {
        RG_LOGE("touch overlay: no memory for button %dx%d\n", w, h);
        return;
    }

    b->pal[ROLE_BORDER] = color;
    /* 开关（白色系）的填充要比按键更暗：否则"白边框+白十字"压在 55% 白填充上对比不足 */
    b->pal[ROLE_FILL] = c565_scale(color, b->is_toggle ? 30 : 55, 100);
    b->pal[ROLE_LABEL] = c565_tint(color, 70, 100);
    b->pal_p[ROLE_BORDER] = 0xFFFF;
    b->pal_p[ROLE_FILL] = c565_scale(color, 95, 100);
    b->pal_p[ROLE_LABEL] = 0xFFFF;

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
    static const uint8_t tier[4]  = {0, A_BORDER, A_FILL, A_LABEL};
    static const uint8_t tierp[4] = {0, A_LABEL, A_FILL_PRESSED, A_LABEL};
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

void rg_overlay_init(void)
{
    if (ready)
        return;

    /* ⚠ 先把用户设置读进来，再碰任何可能失败的分配：
     * 一旦下面某步失败（PSRAM 分配等）而这里没读到，visible 会停在默认 true
     * → 叠加层什么都不画（看起来"按键已关闭"）但触摸命中照旧生效 —— 这就是
     *   "按键不显示、点上去却还有反应"的根因（经验沉淀 §97）。 */
    /* ⚠ 暂时强制常显（忽略 NVS 里的开关值）：蓝牙手柄支持之前，触摸按键是这台设备
     * 唯一的输入源。用户一旦关掉它，就再没有按键能进菜单打开它（左上角那个开关真机
     * 反馈"点不动"，先放一边）——等于把设备锁死。等蓝牙手柄能用了再放开。 */
    visible = true;
    rg_overlay_set_alpha((int)rg_settings_get_number(NS_GLOBAL, SETTING_ALPHA, 100));

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
    btn_count = (n > RG_OVERLAY_MAX_BUTTONS) ? RG_OVERLAY_MAX_BUTTONS : n;   /* 上限保护 */

    for (size_t i = 0; i < n; ++i)
    {
        btns[i].key = map[i].key;
        btns[i].w = map[i].w;
        btns[i].h = map[i].h;
        btns[i].x = map[i].x - map[i].w / 2;
        btns[i].y = map[i].y - map[i].h / 2;
        build_button(&btns[i], overlay_key_color(map[i].key));
    }

    ready = true;
    /* 这一行会出现在串口上：app 切换（进出游戏）时 lcd_init 里会重跑建层，
     * 耗时直接决定黑屏等待时长 —— 所以别把 double/除法/strlen 放回热路径。 */
    RG_LOGI("touch overlay ready: %u buttons, visible=%d, alpha=%d%%, built in %d ms\n",
            (unsigned)btn_count, visible, alpha_pct,
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
    RG_LOGI("touch overlay %s\n", value ? "shown" : "hidden");
}

int rg_overlay_get_alpha(void)
{
    return alpha_pct;
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

void rg_overlay_debug_set_pressed(uint32_t mask)
{
    debug_mask = mask;
}

/* 按下状态：真实按下 + 短暂保持（快速点按也能看到一次反馈） */
static void poll_pressed(void)
{
    uint32_t now = (uint32_t)(rg_system_timer() / 1000);
    uint32_t mask = rg_input_get_pressed_mask() | debug_mask;

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
                     const rg_overlay_btn_t *b, bool cw90)
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

    const bool pr = b->key && (visual_mask & b->key) != 0;
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

void rg_overlay_blit(uint16_t *buf, int stride, int rx, int ry, int rw, int rh)
{
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
        blit_one(buf, stride, rx, ry, rw, rh, 0, &btns[i], false);
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
#define RG_FPS_TEXT_CX 640      /* 逻辑坐标：L 与 R 正中 */
#define RG_FPS_TEXT_CY 60
#define RG_FPS_SCALE   4

static int fps_value = -1;
static char fps_text[8] = "";

void rg_overlay_set_fps(int value)
{
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
    const int px0 = phys_w - ly0 - 8 * RG_FPS_SCALE;   /* 逻辑 y 反向对应物理 x */
    const int px1 = phys_w - ly0;
    const int py0 = lx0;                               /* 逻辑 x 正向对应物理 y */
    const int py1 = lx0 + n * 8 * RG_FPS_SCALE;

    const int cx0 = imax(px0, rx), cx1 = imin(px1, rx + rw);
    const int cy0 = imax(py0, ry), cy1 = imin(py1, ry + rh);
    if (cx0 >= cx1 || cy0 >= cy1)
        return;                                        /* 本块不覆盖数字区域，早退 */

    for (int py = cy0; py < cy1; ++py)
    {
        const int gx = (py - py0) / RG_FPS_SCALE;      /* 逻辑 x -> 字形列 */
        const int ch = gx >> 3, col = gx & 7;
        if (ch >= n)
            continue;
        const uint8_t *gl = glyphs[(uint8_t)fps_text[ch]];
        for (int px = cx0; px < cx1; ++px)
        {
            const int gy = (phys_w - 1 - px - ly0) / RG_FPS_SCALE;   /* 逻辑 y -> 字形行 */
            if (gy < 0 || gy > 7)
                continue;
            if (gl[gy] & (0x80 >> col))                /* 字模 MSB = 最左列 */
                buf[(size_t)(py - ry) * stride + (px - rx)] = color;
        }
    }
}

static void blit_fps(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w)
{
    if (fps_value < 0 || !fps_text[0])
        return;
    const int n = (int)strlen(fps_text);
    const int lx = RG_FPS_TEXT_CX - n * 8 * RG_FPS_SCALE / 2;
    const int ly = RG_FPS_TEXT_CY - 8 * RG_FPS_SCALE / 2;
    draw_fps_text(buf, stride, rx, ry, rw, rh, phys_w, lx + 2, ly + 2, c565(0, 0, 0));      /* 投影 */
    draw_fps_text(buf, stride, rx, ry, rw, rh, phys_w, lx, ly, c565(255, 255, 255));        /* 正文 */
}

/* tab5 专用：面板是原生竖屏，逻辑画面按 90°CW 写进物理帧缓冲。
 * 逻辑 (lx,ly) -> 物理 (px,py) = (phys_w-1-ly, lx)，与显示驱动的映射必须完全一致。
 * 这里不做第二份旋转位图，只在索引上换算 —— 一份数据、两种朝向。 */
void rg_overlay_blit_cw90(uint16_t *buf, int stride, int rx, int ry, int rw, int rh, int phys_w)
{
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
        blit_one(buf, stride, rx, ry, rw, rh, phys_w, &btns[i], true);

    blit_fps(buf, stride, rx, ry, rw, rh, phys_w);
}

#endif /* RG_GAMEPAD_TOUCH_MAP && RG_TOUCH_OVERLAY */
