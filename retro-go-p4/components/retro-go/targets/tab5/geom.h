#pragma once
/* Tab5 屏幕几何的**运行时**真值（0.4.9 单 app 用）。
 *
 * 背景：原本几何是编译期宏，一次编译只装一个方向（targets/tab5/config.h 里的
 * `#if RG_TAB5_ORIENTATION`）。单 app 形态要**一个镜像同时支持横竖** ⇒ 几何必须在运行时按
 * 当前方向取。于是把两套值搬到这里成两行表，由 config.h 把几何宏定义成"读这里的表达式"。
 *
 * ⚠ 这里只放**按方向变化**的 9 项。恒定值（BACKLIGHT / ROTATE / SAFE_AREA /
 *   PARTIAL_UPDATES / DEFAULT_SCALING / INIT）继续留在 config.h 当普通宏 —— 搬进来只会
 *   让改动面变大、收益为零。
 * ⚠ 值必须与改造前 config.h 的两条分支**逐字一致**：双 app 形态下 rg_orient_active() 返回
 *   运行分区标签，取到的值就是原来编译期那套 ⇒ 行为零回归（真机对照 r3/r4 基线验证）。
 *
 * 选择是**粘性**的：第一次调用（= 显示初始化里 lcd_init() 那一刻）定下来，之后不再变。
 * 为什么必须粘：改方向的路径是"写 NVS → 重启"，在重启前那几百毫秒里 NVS 已是新方向，
 * 而另一份显示后端还没 init ⇒ 若每次现查，几何会与正在跑的后端错配（画面错乱）。
 * 锁定后语义干净：写 NVS 只影响下次启动。 */
#include "rg_orient.h"

typedef struct
{
    int l, t, r, b;
} rg_geom_rect_t;

typedef struct
{
    int             driver;          /* 2 = 横屏驱动（90° 映射）3 = 竖屏驱动（线性） */
    int             w, h;            /* 逻辑画面尺寸 */
    rg_geom_rect_t  vis;             /* RG_SCREEN_VISIBLE_AREA（GBA 那档，含肩键行机型） */
    rg_geom_rect_t  vis_ns;          /* RG_SCREEN_VISIBLE_AREA_NO_SHLD（无肩键行机型） */
    int             lcd_rows;        /* 每块行数：竖 32 / 横 16（LCD_BUFFER_LENGTH = w * lcd_rows） */
    int             max_window_h;    /* RG_DISPLAY_MAX_WINDOW_HEIGHT */
    int             lock_scaling;    /* RG_DISPLAY_LOCK_SCALING */
    float           def_custom_zoom; /* RG_DISPLAY_DEFAULT_CUSTOM_ZOOM */
    float           max_custom_zoom; /* RG_DISPLAY_MAX_CUSTOM_ZOOM */
} rg_geom_t;

/* [0] = 竖屏（RG_ORIENT_PORTRAIT）—— 抄自 config.h 竖屏分支（145-191 行，值未改）
 * [1] = 横屏（RG_ORIENT_LANDSCAPE）—— 抄自 config.h 横屏分支（97-143 行，值未改） */
static const rg_geom_t rg_geom_rows[2] = {
    { 3,  720, 1280, {0, 0, 0, 800},     {0, 0, 0, 800},     32, 620, 0, 16.0f, 16.0f },
    { 2, 1280,  720, {280, 120, 280, 120}, {280, 0, 280, 144}, 16, 576, 1,  4.0f,  4.0f },
};

/* 驱动里两个 LCD 缓冲（静态数组，长度必须编译期已知）按**两方向最大值**分配。
 * 上界 = max(竖 720*32, 横 1280*16) = 23040 像素（= 46,080 B）—— 等于竖屏现值，
 * 横屏缓冲从 40KB 长到 46KB（两块共 +12KB 片内 SRAM）。下面有静态断言兜底。 */
/* 两方向的**最大**逻辑尺寸（静态数组按它分配，运行时只用当前方向的实际值）。
 * 竖 720x1280 / 横 1280x720 ⇒ 最大值都是 1280。下面有静态断言兜底，改表时会当场报错。 */
#define RG_GEOM_MAX_W 1280
#define RG_GEOM_MAX_H 1280

#define LCD_BUFFER_LENGTH_MAX (720 * 32)
_Static_assert(720 * 32 <= LCD_BUFFER_LENGTH_MAX && 1280 * 16 <= LCD_BUFFER_LENGTH_MAX,
               "LCD_BUFFER_LENGTH_MAX 必须 >= 两方向的 宽*行数");
/* ⚠ 用 #if 而不是 _Static_assert：C 里 `static const` 结构体成员**不是**整数常量表达式，
 *   _Static_assert(rg_geom_rows[0].w <= …) 会直接报"不是常量"。预处理器反而更严格、也更早。 */
#if 1280 > RG_GEOM_MAX_W || 720 > RG_GEOM_MAX_W
#error "RG_GEOM_MAX_W 太小：静态数组按它分配，必须 >= 两方向的最大逻辑宽（横 1280）"
#endif
#if 1280 > RG_GEOM_MAX_H || 720 > RG_GEOM_MAX_H
#error "RG_GEOM_MAX_H 太小：静态数组按它分配，必须 >= 两方向的最大逻辑高（竖 1280）"
#endif

static inline const rg_geom_t *rg_geom(void)
{
    static const rg_geom_t *settled;               /* 粘性：首次调用（显示 init）时定下来 */
    if (!settled)
    {
        int o = rg_orient_active();
        settled = &rg_geom_rows[o == RG_ORIENT_LANDSCAPE ? 1 : 0];
    }
    return settled;
}
