/* ============================================================================
 * 触摸皮肤 —— 数据 + 面板生成（**唯一真源**）
 * ----------------------------------------------------------------------------
 * 本文件被两处消费：
 *   ① 固件：rg_touch_overlay.c 取配色/图层权重，并把这里生成的面板当"边框图"用；
 *   ② PC 预览：tools/preview-skin.py **解析本文件**（表 + 下面的几何宏），所以
 *      "PC 稿"与"固件将要画的"是同一组数字 —— 别再往预览脚本里抄第二份常量。
 *
 * 面板为什么是程序化生成而不是一张 PNG：
 *   分区色块/细线/凹槽/铭牌全都能用"矩形 + 圆角矩形 + 8x8 字库文字"画出来，
 *   塞 1.8MB 位图进 flash 不划算；生成一张 720x1280 的 PSRAM surface，走现成的
 *   border 通路（rg_display.c），零新增机制。
 *
 * ⚠ 光栅化口径（PC 与固件必须逐位一致）：
 *   圆角矩形 = **硬边**（不做抗锯齿），判据是整数比较：
 *       把像素钳到内缩 r 的核矩形上求距离，dx*dx+dy*dy <= r*r 即在内。
 *   PC 侧 tools/preview-skin.py 的 rrect_hit() 是同一段算术（整数、同序），
 *   所以两张图能逐像素比。按键掩码那套 4x4 超采样抗锯齿仍归 rg_touch_overlay.c
 *   （那是"一次性预渲染"的老机制，本文件不碰）。
 * ==========================================================================*/

#include <string.h>

#include "rg_touch_skin.h"
#include "rg_input.h"       /* rg_input_get_touch_keymap（键位表 = 布局的单一真源） */
#include "rg_touch_overlay.h"  /* rg_overlay_glyph_rows（同一个 8x8 字库，不重复加载） */

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY

/* 供电量圆灯/铭牌定位与覆盖层输出用的 log 宏（rg_system.h 已随 rg_touch_skin.h 包含） */

/* ---------------------------------------------------------------- 几何参数
 * 全部是"面板与按键之间"的关系量，PC 与固件共用。 */
#define RG_TOUCH_PANEL_REF_CTRL_TOP 480   /* 参考控制区顶（GBA）：下面所有偏移都以它为基准 */
#define RG_TOUCH_PANEL_CLUSTER_R    10    /* 分区色块圆角 */
#define RG_TOUCH_PANEL_CLUSTER_PAD_DPAD    22
#define RG_TOUCH_PANEL_CLUSTER_PAD_ACTION  22
#define RG_TOUCH_PANEL_CLUSTER_PAD_SHOULDER 16
#define RG_TOUCH_PANEL_CLUSTER_PAD_SYSTEM  14
#define RG_TOUCH_PANEL_GROOVE_PAD   8     /* 屏幕凹槽比视口外扩多少（让边条读成 bezel 而不是死区） */
/* 画凹槽所需的**最小边条宽度**：窄于此就不画、留黑边。由各机型的窗口算出来，不按机型名写死 ——
 * GBA 0px（画面 720 全宽）、GB/GBC 4x 40px（太窄，画了是脏线；用户定不做元素）、NES 104px（画）。 */
#define RG_TOUCH_PANEL_GROOVE_MIN_SIDE 64
#define RG_TOUCH_PANEL_GROOVE_R     6
#define RG_TOUCH_PANEL_BADGE_PAD_X  56    /* 铭牌底板 = 文字宽 + 此值（横向） */
#define RG_TOUCH_PANEL_BADGE_PAD_Y  30
#define RG_TOUCH_PANEL_BADGE_R      8
#define RG_TOUCH_PANEL_BADGE_SCALE  2     /* 铭牌文字：2 倍 8x8 → 16px 高 */
#define RG_TOUCH_PANEL_BADGE_TRACK  3     /* 字距（px） */

/* 颜色哨兵值 */
#define RG_TOUCH_SKIN_ACCENT 0xFFFFFFFFu  /* "用该机型的点缀色" */
#define RG_TOUCH_SKIN_NONE   0xFFFFFFFEu  /* "这一层不画" */

#define RG_TOUCH_SKIN_MODE_SOLID   0
#define RG_TOUCH_SKIN_MODE_OUTLINE 1

/* ---------------------------------------------------------------- 皮肤表
 * ⚠ 这些表是**被 PC 工具解析的**（tools/preview-skin.py）：
 *   一行一套皮肤、一行一套配色，字段顺序别改（改了要同步改解析器），
 *   汉字注释可以写，字符串值只能放 ASCII（设备字库只有 8x8 ASCII）。 */
/* ⚠ 成品配色（2026-10-07 定稿）：强度按用户选择——**分区框明显可见**（底+描边都保留）、
   四类元素全留（分区框 / 凹槽 / 铭牌 / 画面-控制区分界线）。
   565 量化后"分区填充 vs 面板底"的 G 通道差都做到 ≥8 级（≈12% 亮度），这是真机低亮度下
   还能看见的下限 —— 原来的 1~3 级（3~5%）在 PC 满亮度图上看得见、真机上就是纯黑。
   D 套的分区线/分界线/铭牌线用 **每机型点缀色**（GBA 靛蓝 / GB 绿 / NES 红），随核心自动变。 */
#define RG_TOUCH_SKINS { \
    /* id  名字(ASCII)            短名       面板底   分区填充  分区线    分隔线    线粗 铭牌底   铭牌线   铭牌字    模式 描边 圆角% 填充% 标签%  标签覆盖    边框α 填充α 标签α 用机型点缀色 */ \
    {"A", "Deep Grey + Amber",  "Amber",   0x14171C, 0x2A3540, 0x8A6420, 0x9A7028, 2, 0x2A3540, 0x8A6420, 0xE8A22C, 0, 3, 18, 42, 72, RG_TOUCH_SKIN_NONE,   230, 128, 255, 0}, \
    {"B", "Minimal Outline",   "Outline", 0x0E1013, RG_TOUCH_SKIN_NONE, 0x505C6A, 0x3A4552, 1, RG_TOUCH_SKIN_NONE, 0x505C6A, 0x9AA3B0, 1, 3, 18,  0, 55, RG_TOUCH_SKIN_NONE,   230,   0, 255, 0}, \
    {"C", "Amber Retro",       "Retro",   0x17110A, 0x34281A, 0x8A6420, 0xA87A28, 2, 0x34281A, 0x8A6420, 0xFFC96B, 0, 3, 18, 30, 85, 0xFFD98A,             230, 128, 255, 0}, \
    {"D", "Console Colors",    "Console", 0x161B26, 0x2A3540, RG_TOUCH_SKIN_ACCENT, RG_TOUCH_SKIN_ACCENT, 2, 0x2A3540, RG_TOUCH_SKIN_ACCENT, RG_TOUCH_SKIN_ACCENT, 0, 3, 18, 45, 80, RG_TOUCH_SKIN_NONE, 230, 128, 255, 1}, \
}

/* 每套皮肤的每键配色（槽位序见 rg_touch_skin.h 的 rg_skin_slot_t）：
 *      UP     DOWN   LEFT   RIGHT   A      B      X      Y      L      R      SELECT START  MENU   SWAP */
#define RG_TOUCH_SKIN_KEYS { \
    {0x8B94A2, 0x8B94A2, 0x8B94A2, 0x8B94A2, 0xC4574B, 0xC4A63C, 0x4C74B4, 0x5A8F62, 0x99A3B0, 0x99A3B0, 0x6D7684, 0x6D7684, 0xE8A22C, 0xE8A22C}, \
    {0x7C8592, 0x7C8592, 0x7C8592, 0x7C8592, 0xA8AFBA, 0xA8AFBA, 0xA8AFBA, 0xA8AFBA, 0x7C8592, 0x7C8592, 0x7C8592, 0x7C8592, 0xD08A22, 0xD08A22}, \
    {0xB98A45, 0xB98A45, 0xB98A45, 0xB98A45, 0xD9614F, 0xD9B24A, 0x6E86C4, 0x86A85E, 0xA98F63, 0xA98F63, 0x8A7350, 0x8A7350, 0xFFB84D, 0xFFB84D}, \
    {0x8B94A2, 0x8B94A2, 0x8B94A2, 0x8B94A2, 0xD24636, 0xE0C03A, 0x3D7CD6, 0x4FA65C, 0x99A3B0, 0x99A3B0, 0x6D7684, 0x6D7684, RG_TOUCH_SKIN_ACCENT, RG_TOUCH_SKIN_ACCENT}, \
}

/* 机型表：configNs → 铭牌文案 + 点缀色（D 套"每台机器自己的颜色"就用它） */
#define RG_TOUCH_CONSOLES { \
    {"gba", "ADVANCE",          0x5A4FA2}, \
    {"gb",  "GAME BOY",         0x6E8F52}, \
    {"gbc", "GAME BOY COLOR",   0x3F8F8F}, \
    {"nes", "NES",              0xB5342B}, \
    {"snes", "SUPER NES",       0x8E5FA8}, \
    {"sms", "MASTER SYSTEM",    0x2F6FA8}, \
    {"gg",  "GAME GEAR",        0x7A4FA8}, \
    {"col", "COLECOVISION",     0x8A6A2C}, \
    {"pce", "PC ENGINE",        0xC46A2C}, \
    {"gw",  "GAME & WATCH",     0xB04A2C}, \
    {"lnx", "LYNX",             0x6A8F3F}, \
    {"md",  "MEGA DRIVE",       0x2F4FA8}, \
    {"",    "RETRO-GO",         0xE8A22C}, \
}

typedef struct
{
    const char *id;
    const char *name;
    const char *short_name;
    uint32_t panel_bg, cluster_fill, cluster_line, divider;
    int divider_h;
    uint32_t badge_fill, badge_line, badge_text;
    int mode;                    /* RG_TOUCH_SKIN_MODE_* */
    int border_px, radius_pct, fill_pct, label_pct;
    uint32_t label_override;
    uint8_t tier_border, tier_fill, tier_label;   /* 覆盖率权重（B 套 fill=0 → 线框） */
    int accent_from_console;
} rg_touch_skin_data_t;

typedef struct { const char *id; const char *badge; uint32_t accent; } rg_touch_console_t;

static const rg_touch_skin_data_t skins[] = RG_TOUCH_SKINS;
static const uint32_t skin_keys[][RG_SKIN_SLOT_COUNT] = RG_TOUCH_SKIN_KEYS;
static const rg_touch_console_t consoles[] = RG_TOUCH_CONSOLES;

_Static_assert(sizeof(skins) / sizeof(skins[0]) == RG_TOUCH_SKIN_COUNT, "皮肤表行数与 RG_TOUCH_SKIN_COUNT 不一致");
_Static_assert(sizeof(skin_keys) / sizeof(skin_keys[0]) == RG_TOUCH_SKIN_COUNT, "配色表行数与皮肤数不一致");

/* ---------------------------------------------------------------- 颜色工具
 * ⚠ 口径：皮肤表里写的是 0xRRGGBB（人能读），但**面板真正用到的颜色必须先进 565 再
 *   派生**（PC 侧 q565() 是同一件事）—— 否则 PC 稿与真机在暗部/过渡色上会有偏差。 */
static uint8_t q5(int v) { return (uint8_t)((v >> 3) * 255 / 31); }
static uint8_t q6(int v) { return (uint8_t)((v >> 2) * 255 / 63); }

/* 888 → 量化到 565 → 还原 888（"面板只能显示 565"这一步必须在派生之前做） */
static uint32_t q565(uint32_t rgb)
{
    const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((uint32_t)q5(r) << 16) | ((uint32_t)q6(g) << 8) | (uint32_t)q5(b);
}

static uint16_t to565(uint32_t rgb)
{
    const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t shade(uint32_t rgb, int pct)
{
    const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((uint32_t)(r * pct / 100) << 16) | ((uint32_t)(g * pct / 100) << 8) | (uint32_t)(b * pct / 100);
}

static uint32_t tint(uint32_t rgb, int pct)
{
    const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((uint32_t)(r + (255 - r) * pct / 100) << 16) |
           ((uint32_t)(g + (255 - g) * pct / 100) << 8) |
           (uint32_t)(b + (255 - b) * pct / 100);
}

/* ---------------------------------------------------------------- 查询 */
int rg_touch_skin_count(void) { return (int)(sizeof(skins) / sizeof(skins[0])); }

int rg_touch_skin_clamp(int idx)
{
    const int n = rg_touch_skin_count();
    if (idx < 0) return 0;
    if (idx >= n) return n - 1;
    return idx;
}

const char *rg_touch_skin_name(int idx)       { return skins[rg_touch_skin_clamp(idx)].name; }
const char *rg_touch_skin_short_name(int idx) { return skins[rg_touch_skin_clamp(idx)].short_name; }

int rg_touch_skin_slot_for_key(rg_key_t key)
{
    switch (key)
    {
        case RG_KEY_UP:     return RG_SKIN_SLOT_UP;
        case RG_KEY_DOWN:   return RG_SKIN_SLOT_DOWN;
        case RG_KEY_LEFT:   return RG_SKIN_SLOT_LEFT;
        case RG_KEY_RIGHT:  return RG_SKIN_SLOT_RIGHT;
        case RG_KEY_A:      return RG_SKIN_SLOT_A;
        case RG_KEY_B:      return RG_SKIN_SLOT_B;
        case RG_KEY_X:      return RG_SKIN_SLOT_X;
        case RG_KEY_Y:      return RG_SKIN_SLOT_Y;
        case RG_KEY_L:      return RG_SKIN_SLOT_L;
        case RG_KEY_R:      return RG_SKIN_SLOT_R;
        case RG_KEY_SELECT: return RG_SKIN_SLOT_SELECT;
        case RG_KEY_START:  return RG_SKIN_SLOT_START;
        case RG_KEY_MENU:   return RG_SKIN_SLOT_MENU;
        /* 那颗 X/Y ↔ L/R 调换按钮不是游戏键（key=RG_KEY_NONE），它的颜色走 SWAP 槽 */
        default:            return RG_SKIN_SLOT_SWAP;
    }
}

static const rg_touch_console_t *console_entry(const char *console_id)
{
    const size_t n = sizeof(consoles) / sizeof(consoles[0]);
    if (console_id && *console_id)
        for (size_t i = 0; i < n; ++i)
            if (consoles[i].id[0] && strcmp(consoles[i].id, console_id) == 0)
                return &consoles[i];
    return &consoles[n - 1];   /* 兜底那行（id 为空） */
}

const char *rg_touch_skin_badge(const char *console_id)
{
    return console_entry(console_id)->badge;
}

uint16_t rg_touch_skin_accent(int skin_idx, const char *console_id)
{
    const rg_touch_skin_data_t *s = &skins[rg_touch_skin_clamp(skin_idx)];
    const uint32_t a = s->accent_from_console ? console_entry(console_id)->accent : 0xE8A22C;
    return to565(q565(a));
}

uint16_t rg_touch_skin_key_color(int skin_idx, int slot, const char *console_id)
{
    if (slot < 0 || slot >= RG_SKIN_SLOT_COUNT)
        slot = RG_SKIN_SLOT_SWAP;
    const uint32_t c = skin_keys[rg_touch_skin_clamp(skin_idx)][slot];
    if (c == RG_TOUCH_SKIN_ACCENT)
        return rg_touch_skin_accent(skin_idx, console_id);
    return to565(q565(c));
}

void rg_touch_skin_tiers(int skin_idx, uint8_t out_tiers[3])
{
    const rg_touch_skin_data_t *s = &skins[rg_touch_skin_clamp(skin_idx)];
    out_tiers[0] = s->tier_border;
    out_tiers[1] = s->tier_fill;
    out_tiers[2] = s->tier_label;
}

void rg_touch_skin_button_recipe(int skin_idx, int *radius_pct, int *fill_pct, int *label_pct, bool *outline_only)
{
    const rg_touch_skin_data_t *s = &skins[rg_touch_skin_clamp(skin_idx)];
    if (radius_pct)   *radius_pct = s->radius_pct;
    if (fill_pct)     *fill_pct = s->fill_pct;
    if (label_pct)    *label_pct = s->label_pct;
    if (outline_only) *outline_only = (s->mode == RG_TOUCH_SKIN_MODE_OUTLINE);
}

bool rg_touch_skin_label_color(int skin_idx, uint16_t *out)
{
    const rg_touch_skin_data_t *s = &skins[rg_touch_skin_clamp(skin_idx)];
    if (s->label_override == RG_TOUCH_SKIN_NONE)
        return false;
    if (out)
        *out = to565(q565(s->label_override));
    return true;
}

/* ---------------------------------------------------------------- 光栅化
 * 只做两件事：硬边圆角矩形（填充 + 1px 描边）与 8x8 字库文字。
 * ⚠ 与 PC 侧 tools/preview-skin.py 的 rrect_hit()/draw_text_run() 是同一段算术。 */
static inline bool rrect_hit(int px, int py, int x, int y, int w, int h, int r)
{
    if (w <= 0 || h <= 0)
        return false;
    const int half = (w < h ? w : h) / 2;
    if (r > half) r = half;
    if (r < 0) r = 0;
    const int x1 = x + w - 1 - r, y1 = y + h - 1 - r;
    const int cx = px < x + r ? x + r : (px > x1 ? x1 : px);
    const int cy = py < y + r ? y + r : (py > y1 ? y1 : py);
    const int dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

static void panel_rrect(rg_surface_t *s, int x, int y, int w, int h, int r,
                        uint16_t fill, bool have_fill, uint16_t line, bool have_line)
{
    uint16_t *buf = (uint16_t *)s->data;
    const int stride = s->stride / 2;
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int x1 = x + w > s->width ? s->width : x + w;
    const int y1 = y + h > s->height ? s->height : y + h;
    for (int py = y0; py < y1; ++py)
    {
        for (int px = x0; px < x1; ++px)
        {
            if (!rrect_hit(px, py, x, y, w, h, r))
                continue;
            if (have_line && !rrect_hit(px, py, x + 1, y + 1, w - 2, h - 2, r - 1))
                buf[(size_t)py * stride + px] = line;      /* 1px 描边（画在填充之上，与 PC 同序） */
            else if (have_fill)
                buf[(size_t)py * stride + px] = fill;
        }
    }
}

static void panel_rect(rg_surface_t *s, int x, int y, int w, int h, uint16_t color)
{
    uint16_t *buf = (uint16_t *)s->data;
    const int stride = s->stride / 2;
    for (int py = y < 0 ? 0 : y; py < y + h && py < s->height; ++py)
        for (int px = x < 0 ? 0 : x; px < x + w && px < s->width; ++px)
            buf[(size_t)py * stride + px] = color;
}

/* 8x8 字库文字（字距 track，按 8 行格居中 —— 与 PC 的 draw_text_run 同口径）。
 * 字形从覆盖层取（同一个字库、同一份加载结果，见 rg_overlay_glyph_rows 的注释）。 */
static void panel_text(rg_surface_t *s, int cx, int cy, int scale, int track, const char *text, uint16_t color)
{
    if (!text || !*text)
        return;
    const int n = (int)strlen(text);
    const int total = n * 8 * scale + (n - 1) * track;
    const int x0 = cx - total / 2;
    const int y0 = cy - (8 * scale) / 2;
    for (int i = 0; i < n; ++i)
    {
        const uint8_t *rows = rg_overlay_glyph_rows((unsigned char)text[i]);
        if (!rows)
            continue;
        for (int gy = 0; gy < 8; ++gy)
        {
            if (!rows[gy])
                continue;
            for (int gx = 0; gx < 8; ++gx)
            {
                if (!(rows[gy] & (0x80 >> gx)))
                    continue;
                panel_rect(s, x0 + i * (8 * scale + track) + gx * scale, y0 + gy * scale, scale, scale, color);
            }
        }
    }
}

/* ---------------------------------------------------------------- 布局（锚定规则）
 * 上三排随控制区顶下移 dy，系统键行钉死在屏底 —— GBA（dy=0）与现状**逐字节相同**。
 * 分区色块、铭牌位置都由这里算出来，PC 侧用同一套规则（见 preview-skin.py 的 layout()）。
 * 键位从 rg_input_get_touch_keymap() 取（与触摸命中/掩码同一个真源）。 */
typedef struct { int x0, y0, x1, y1; } skin_box_t;

static void layout_compute(const char *console_id, int ctrl_top, skin_box_t clusters[4], int *led_cy,
                           int *badge_cy, int led_cy_in, int led_r)
{
    size_t n = 0;
    const rg_keymap_touch_t *map = rg_input_get_touch_keymap(&n);
    /* 上三排的"避让画面"下移量 —— **恒为 0**。
     * ⚠ 真实按键（btns[]）用的是键位表的固定坐标、**不加 dy**，所以面板分区框一旦单独平移，
     *   就会与按键错位（真机表现：框在下面、键在上面）。要避让就必须同时改键位表
     *   （targets/tab5/touch_layout.h）+ 掩码 + 命中判定 —— 那是"挪按键"，不是"挪面板"。
     * GB 4x 去掉肩键行后按键顶(653)仍在画面底(576)之下，本来就不需要避让；
     * 若将来某个机型真的压到画面，下面的自检会打 WARNING，改键位表即可。 */
    const int dy = 0;
    const int pads[4] = {RG_TOUCH_PANEL_CLUSTER_PAD_DPAD, RG_TOUCH_PANEL_CLUSTER_PAD_ACTION,
                         RG_TOUCH_PANEL_CLUSTER_PAD_SHOULDER, RG_TOUCH_PANEL_CLUSTER_PAD_SYSTEM};
    int upper_bottom = 0, sys_top = 1 << 30;
    for (int c = 0; c < 4; ++c)
    {
        clusters[c].x0 = 1 << 30; clusters[c].y0 = 1 << 30;
        clusters[c].x1 = -(1 << 30); clusters[c].y1 = -(1 << 30);
    }

    for (size_t i = 0; i < n && map; ++i)
    {
        /* 该机型没有的键（GB/GBC 无 L/R 肩键）不进分区框 —— 那一组自然为空，
         * 绘制端靠 w<=0||h<=0 跳过（见面板生成的第 ④ 步）。 */
        if (rg_touch_key_hidden(console_id, map[i].key))
            continue;
        const bool is_system = (map[i].key == RG_KEY_SELECT || map[i].key == RG_KEY_START || map[i].key == RG_KEY_MENU);
        int x = map[i].x - map[i].w / 2;
        int y = map[i].y - map[i].h / 2 + (is_system ? 0 : dy);
        int w = map[i].w, h = map[i].h;

        if (is_system)
        {
            if (y < sys_top) sys_top = y;
            if (y + h > upper_bottom && false) { }
        }
        else if (y + h > upper_bottom)
        {
            upper_bottom = y + h;
        }

        int c = 0;
        switch (map[i].key)
        {
            case RG_KEY_UP: case RG_KEY_DOWN: case RG_KEY_LEFT: case RG_KEY_RIGHT: c = 0; break;
            case RG_KEY_X: case RG_KEY_Y: case RG_KEY_A: case RG_KEY_B: c = 1; break;
            case RG_KEY_L: case RG_KEY_R: c = 2; break;
            default: c = is_system ? 3 : 2; break;   /* 定不下来就归到相邻那一组（不该发生） */
        }
        const int p = pads[c];
        if (x - p < clusters[c].x0) clusters[c].x0 = x - p;
        if (y - p < clusters[c].y0) clusters[c].y0 = y - p;
        if (x + w + p > clusters[c].x1) clusters[c].x1 = x + w + p;
        if (y + h + p > clusters[c].y1) clusters[c].y1 = y + h + p;
    }

    /* 肩键那一组要连上中间那颗调换按钮（它不在键位表里，几何在 touch_layout.h）。
     * GB/GBC 没有肩键行 → 那颗按钮也不存在 → 整段跳过。 */
    if (rg_touch_has_shoulders(console_id))
    {
        const int sx = RG_TAB5_SWAP_X - RG_TAB5_SWAP_W / 2 - RG_TOUCH_PANEL_CLUSTER_PAD_SHOULDER;
        const int sy = RG_TAB5_SWAP_Y - RG_TAB5_SWAP_H / 2 + dy - RG_TOUCH_PANEL_CLUSTER_PAD_SHOULDER;
        const int sx1 = RG_TAB5_SWAP_X + RG_TAB5_SWAP_W / 2 + RG_TOUCH_PANEL_CLUSTER_PAD_SHOULDER;
        const int sy1 = RG_TAB5_SWAP_Y + RG_TAB5_SWAP_H / 2 + dy + RG_TOUCH_PANEL_CLUSTER_PAD_SHOULDER;
        if (sx < clusters[2].x0) clusters[2].x0 = sx;
        if (sy < clusters[2].y0) clusters[2].y0 = sy;
        if (sx1 > clusters[2].x1) clusters[2].x1 = sx1;
        if (sy1 > clusters[2].y1) clusters[2].y1 = sy1;
    }

    /* 圆灯与铭牌在这段竖向空间里**各占一半、各自居中**（上半 = 铭牌，下半 = 圆灯）。
     * 旧规则是"灯居中、铭牌挤在灯上方"，真机上两者只隔十几像素（用户反馈"太挤"）。
     * led_cy_in > 0 时仍以它为准（覆盖层可以显式钉死灯位）。 */
    *led_cy = led_cy_in;
    if (sys_top > upper_bottom)
    {
        const int mid = (upper_bottom + sys_top) / 2;
        if (led_cy_in <= 0)
            *led_cy = (mid + sys_top) / 2;      /* 下半区居中 */
        *badge_cy = (upper_bottom + mid) / 2;   /* 上半区居中 */
    }
    else
    {
        *badge_cy = (upper_bottom + (*led_cy - led_r)) / 2;   /* 退化情形：按老规则兜底 */
    }
}

/* 覆盖层与显示驱动要跟面板用**同一套**几何（各写一份常量 = 必然漂移）：
 * 圆灯中心 y 由这里算，调用方传自己的控制区顶与灯半径。 */
int rg_touch_skin_led_cy(int ctrl_top, int led_r)
{
    skin_box_t clusters[4];
    int led_cy = 0, badge_cy = 0;
    layout_compute(NULL, ctrl_top, clusters, &led_cy, &badge_cy, 0, led_r);   /* 传 0 = 用推导值；NULL = 不过滤按键（灯位与肩键行无关） */
    return led_cy;
}

/* 面板底色（已量化成 565）：驱动擦"电量灯条带"时拿它当背景。
 * ⚠ 不能用纯黑 —— 非黑面板上那条带会割裂画面（真机反馈）。 */
uint16_t rg_touch_skin_panel_bg565(int skin_idx)
{
    return to565(skins[rg_touch_skin_clamp(skin_idx)].panel_bg);
}

/* ---------------------------------------------------------------- 面板生成 */
static rg_surface_t *panel;         /* 720x1280，原地复用（见 .h 的所有权说明） */
static int panel_ctrl_top = -1;
static int panel_skin = -1;
static int panel_win_x = -1, panel_win_w = -1;
static char panel_console[16];

rg_surface_t *rg_touch_panel_get(int skin_idx, const char *console_id, int ctrl_top,
                                 int win_x, int win_w, int led_cy, int led_r)
{
    skin_idx = rg_touch_skin_clamp(skin_idx);
    const rg_touch_skin_data_t *s = &skins[skin_idx];

    if (!panel)
    {
        panel = rg_surface_create(RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, RG_PIXEL_565_LE, MEM_SLOW);
        if (!panel)
        {
            RG_LOGE("touch skin: panel allocation failed (%dx%d)\n", RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT);
            return NULL;
        }
        panel_ctrl_top = -1;      /* 强制首次整画 */
    }

    const char *cid = console_id ? console_id : "";
    const bool same = (panel_ctrl_top == ctrl_top) && (panel_skin == skin_idx) &&
                      (panel_win_x == win_x) && (panel_win_w == win_w) &&
                      (strncmp(panel_console, cid, sizeof(panel_console) - 1) == 0);
    if (same)
        return panel;             /* 没变就不重画（皮肤切换/机型切换才重画） */

    const int64_t t0 = rg_system_timer();
    panel_ctrl_top = ctrl_top;
    panel_skin = skin_idx;
    panel_win_x = win_x;
    panel_win_w = win_w;
    strncpy(panel_console, cid, sizeof(panel_console) - 1);
    panel_console[sizeof(panel_console) - 1] = 0;

    /* 派生色：全部先过 565 量化，PC 侧同口径 */
    const uint32_t accent = (s->accent_from_console ? console_entry(cid)->accent : 0xE8A22C);
    const uint32_t panel_bg = q565(s->panel_bg);
    /* ⚠ 屏幕四周的**凹槽与分界线已取消**（用户 2026-10-07："屏幕不需要主题"）——
     *   这里不再派生 groove_fill / groove_line / divider（死变量会触发 -Wunused-variable）。
     *   皮肤元素只剩：分区框 / 铭牌 / 圆灯。要回滚就翻 git 历史，别把不画的变量留在代码里。
     * ACCENT 一律解释成"该机型的点缀色**本身**"（GBA 靛蓝 / GB 绿 / NES 红）。
     * ⚠ 不要再给它套 shade()/tint() 变暗 —— 那等于把"明显可见"的点缀色又压回看不见
     * （真机教训：分区框那档强度就是被这种"再暗一点"吃掉的）。
     * D 套的表里 cluster_line / divider / badge_line / badge_text 都写 ACCENT。 */
    const uint32_t accent565 = q565(accent);
    const uint32_t cluster_fill = s->cluster_fill == RG_TOUCH_SKIN_NONE ? 0 : q565(s->cluster_fill);
    const uint32_t cluster_line = s->cluster_line == RG_TOUCH_SKIN_NONE ? 0
                                : s->cluster_line == RG_TOUCH_SKIN_ACCENT ? accent565 : q565(s->cluster_line);
    const uint32_t badge_fill = s->badge_fill == RG_TOUCH_SKIN_NONE ? 0 : q565(s->badge_fill);
    const uint32_t badge_line = s->badge_line == RG_TOUCH_SKIN_ACCENT ? accent565 : q565(s->badge_line);
    const uint32_t badge_text = s->badge_text == RG_TOUCH_SKIN_ACCENT ? accent565 : q565(s->badge_text);

    skin_box_t clusters[4];
    int led_cy_used = 0, badge_cy = 0;
    layout_compute(console_id, ctrl_top, clusters, &led_cy_used, &badge_cy, led_cy, led_r);

    /* ① 整屏底色 */
    panel_rect(panel, 0, 0, RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, to565(panel_bg));

    /* ② 屏幕凹槽 —— **已取消**（用户 2026-10-07："把所有支持机种的屏幕附近的边、属于皮肤的部分
     *    都去掉，屏幕不需要主题"）。所以画面四周不再有任何皮肤元素：凹槽/描边全不画，
     *    GBA 全宽、GB/GBC 的 40px 窄边、NES 的 104px 宽边，一律只是面板底色（黑边）。
     *    groove_fill / groove_line 与 RG_TOUCH_PANEL_GROOVE_* 常量保留在文件里（结构不动），
     *    但不再参与绘制 —— 以后想给某个机型加回"屏幕嵌在面板里"的读感，就在这里重画。 */

    /* ③ 画面与控制区的分界线 —— **已取消**（用户 2026-10-07：全机种都不要画面底部那条亮线，
     *    画面直接贴到控制区底色上，不再用横线分隔）。
     *    s->divider / s->divider_h 字段在四套皮肤表里保留（结构不动），但当前不再绘制。 */

    /* ④ 四个分区色块（B 套是线框皮肤：不填充，只留 1px 线） */
    for (int c = 0; c < 4; ++c)
    {
        const int x = clusters[c].x0, y = clusters[c].y0;
        const int w = clusters[c].x1 - clusters[c].x0, h = clusters[c].y1 - clusters[c].y0;
        if (w <= 0 || h <= 0)
            continue;
        panel_rrect(panel, x, y, w, h, RG_TOUCH_PANEL_CLUSTER_R,
                    to565(cluster_fill), cluster_fill != 0, to565(cluster_line), cluster_line != 0);
    }

    /* ⑤ 机型铭牌（文案随核心变） */
    const char *badge = console_entry(cid)->badge;
    const int tw = (int)strlen(badge) * 8 * RG_TOUCH_PANEL_BADGE_SCALE +
                   ((int)strlen(badge) - 1) * RG_TOUCH_PANEL_BADGE_TRACK;
    const int pw = tw + RG_TOUCH_PANEL_BADGE_PAD_X;
    const int ph = 8 * RG_TOUCH_PANEL_BADGE_SCALE + RG_TOUCH_PANEL_BADGE_PAD_Y;
    if (badge_fill)
        panel_rrect(panel, 360 - pw / 2, badge_cy - ph / 2, pw, ph, RG_TOUCH_PANEL_BADGE_R,
                    to565(badge_fill), true, to565(badge_line), true);
    panel_text(panel, 360, badge_cy, RG_TOUCH_PANEL_BADGE_SCALE, RG_TOUCH_PANEL_BADGE_TRACK,
               badge, to565(badge_text));

    RG_LOGI("touch skin: panel rebuilt for skin %s / console '%s' (ctrl_top=%d, badge_y=%d, %d ms)\n",
            s->id, cid, ctrl_top, badge_cy, (int)((rg_system_timer() - t0) / 1000));
    return panel;
}

#endif /* defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY */
