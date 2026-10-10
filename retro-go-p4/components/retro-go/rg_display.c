#include "rg_system.h"
#include "rg_display.h"
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
#include "rg_touch_overlay.h"   /* 叠加层改内容后请求"整屏重推"（见 write_update） */
#include "rg_touch_skin.h"      /* rg_touch_has_shoulders：按机型选"画面可用区"档位 */
#endif

#include <stdlib.h>
#include <string.h>

#ifndef LCD_BUFFER_LENGTH
// In pixels. 大屏 target（如 tab5 1280 宽）可覆盖它来减少每帧的推送次数：本驱动是"算一块推一块"，
// 4 行/次意味着一帧推 180 次，播游戏时面板 DMA 会持续处于繁忙态，触发下面的重试。
#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * 4)
#endif

// static rg_display_driver_t driver;
static rg_task_t *display_task_queue;

/* 显示队列深度。⚠ 已实测：把深度提到 2（配合 gbsp 的三缓冲轮转）能让"真正画出来的帧数"
 * 从 15/秒 翻到 30/秒，画面明显更顺 —— 但在这块板上会周期性把显示路径楔死：
 * 现象 = 先流畅、随后画面定格、不写新 crash.log、看门狗不报，只能断电恢复
 * （DSI/PSRAM 总线仲裁饿死，与 PPA 直写帧缓冲那次同一类）。故强制保持 1。
 * 若将来要再试：必须同时给出限流方案（不要整屏突发），并做成运行时开关，见移植笔记。 */
#ifndef RG_DISPLAY_QUEUE_LEN
/* ⚠ 深度 1 是**安全默认**：rg_task_send 用 portMAX_DELAY，队列满时模拟器线程无限阻塞，
 *   于是"模拟器生产"与"显示推送"被串行化 —— 这正是满屏负载只有 ~15fps 的机制。
 *   实测把深度提到 2 能让真正画出来的帧数翻倍（15→30/秒），但当年会周期性把显示路径楔死
 *   （画面定格、只能断电；DSI/PSRAM 总线饿死那一类），故一直保持 1。
 *   2026-09-25 夜查清了背后机制（见 docs/archive/NIGHT-2026-09-25-DISPLAY.md）：IDF 的 DPI 驱动在
 *   DMA2D 忙时是**丢弃**本次绘制（不是等待），丢帧率一度达 51%；同时找到了官方指路的
 *   AXI-ICM QoS 提权（已实施）。于是"深度 2"值得再试一次 —— 但仍必须配限流/上限，
 *   所以做成**独立镜像**（dist/retro-go-p2.6.8-depth2.img）而不是改默认值：
 *   dist/p2.6.7 = QoS + 有界重试（安全档）；dist/p2.6.8 = 在前者之上再把本值设为 2（实验档）。 */
#define RG_DISPLAY_QUEUE_LEN 1   /* 安全默认；实验档见上方注释（dist/retro-go-p2.6.8-depth2.img） */
#endif
static rg_display_counters_t counters;
static rg_display_config_t config;
static rg_surface_t *osd;
static rg_surface_t *border;
static bool border_foreign;   /* border 指向的是别人（rg_touch_skin.c）持有的内存面板 */
static rg_display_t display;
static int16_t map_viewport_to_source_x[RG_GEOM_MAX_W + 1];   /* 0.4.9：按两方向最大值分配 */
static int16_t map_viewport_to_source_y[RG_GEOM_MAX_H + 1];
static uint32_t screen_line_checksum[RG_GEOM_MAX_H + 1];

/* 驱动侧丢块的上报入口（在驱动里被调用）。丢块 = 这一块**没推到面板**，
 * 而行校验和是在推送之前就写好的 —— 不把校验和退回"未知"，这块就永久定格：
 *   游戏里每帧内容都变 → 下一帧照样重推 → 看不出来；
 *   静态画面（存档选择/名字输入/菜单）只有光标那几行变 → 那几行一丢就再也推不出去
 *   → 用户看到的是"方向键没反应、选不了存档/字母"。
 * 真机反馈（2026-10-03，恶魔城晓月/月轮/白夜 三作）：存档选择与名字输入画面无法交互，
 * 就是这么来的。校验和清零 = 强制下一帧重推，与 lcd_init/GUI 重绘的清零约定一致。 */
uint32_t rg_display_push_drops = 0;      /* 累计丢块次数（诊断用） */
uint32_t rg_display_push_hash = 0;       /* 累计"推给面板的像素"哈希（诊断：显示层到底推没推新像素） */
uint32_t rg_display_push_blocks = 0;     /* 累计推送次数（诊断） */
uint32_t rg_display_dirty_lines = 0;     /* 累计"被判定为变了"的扫描行数（诊断：显示层认没认出变化） */
uint32_t rg_display_push_redirty = 0;    /* 因丢块被退回重推的行数（诊断用） */

#ifndef RG_DISPLAY_PUSH_REDIRTY
#define RG_DISPLAY_PUSH_REDIRTY 1
#endif

/* ── P2-2：显示通路「只提交变化行」（默认关，放在 SD 卡开关后面）────────────────────
 * 现状：一个 32 行的块里只要有 1 行变了，整块 32 行都会走一遍转置+推屏（LCD_BUFFER_LENGTH
 * = 720×32，见 tab5 config.h）。改动：块内**按变化行的连续段**分别设置窗口推送，
 * 没变的行不推（不推的行只是我们不把它送进面板，缓冲区内容照旧丢弃）。
 *
 * 为什么放 SD 卡开关后面：这是显示通路的改动，一旦真机上出问题必须能**不重刷固件**退回；
 * 而且 A/B 对比需要"同一份代码两种行为"。开关 = <config>/display-dirty-rows.txt **存在即启用**
 * （文件内容不解析），出厂默认关闭 = 现行为。
 *
 * 编译期默认值 RG_DISPLAY_DIRTY_ROWS_DEFAULT 只给无人值守真机实验用（默认 0）。
 * ⚠ 行级推送的安全前提：垂直滤波（filter_y）会用 i±1 行混合出"重复行"，所以变化掩码
 *   必须**向外膨胀 1 行**再推送；否则"自身校验和没变、邻居变了"的重复行会漏推 → 残影。
 *   膨胀由下面的 RG_DIRTY_ROW_DILATE 分支保证，别删。 */
#ifndef RG_DISPLAY_DIRTY_ROWS_DEFAULT
#define RG_DISPLAY_DIRTY_ROWS_DEFAULT 0
#endif
/* 单次块内最多多少行（掩码数组上限）；块大小由 LCD_BUFFER_LENGTH/draw_width 决定，
 * 720 宽时是 32 行。给到 128 足够覆盖窄窗口场景，超了就把块切小（见调用处）。 */
#define RG_DIRTY_ROW_MAX 128
static int rg_display_dirty_rows_state = -1;   /* -1 = 还没判定 */

static bool rg_display_dirty_rows_on(void)
{
    if (rg_display_dirty_rows_state < 0)
    {
        rg_display_dirty_rows_state = RG_DISPLAY_DIRTY_ROWS_DEFAULT ? 1 : 0;
        bool sd = rg_storage_exists(RG_BASE_PATH_CONFIG "/display-dirty-rows.txt");
        if (sd)
            rg_display_dirty_rows_state = 1;
        RG_LOGI("display: 只提交变化行 = %s（编译期默认 %d，SD 开关 %s）\n",
                rg_display_dirty_rows_state ? "开" : "关", RG_DISPLAY_DIRTY_ROWS_DEFAULT,
                sd ? "有" : "无");
    }
    return rg_display_dirty_rows_state == 1;
}

void rg_display_push_failed(int top, int lines)
{
    if (lines <= 0)
        return;
#if !RG_DISPLAY_PUSH_REDIRTY
    /* 宿主 A/B 用：模拟"没有退回校验和"的旧版本（丢块 = 那几行永久定格）。 */
    rg_display_push_drops++;
    return;
#endif
    rg_display_push_drops++;
    const int last = RG_MIN(top + lines, (int)RG_COUNT(screen_line_checksum) - 1);
    for (int y = RG_MAX(top, 0); y < last; ++y)
    {
        screen_line_checksum[y] = 0;
        rg_display_push_redirty++;
    }
}

#define LINE_IS_REPEATED(Y) (map_viewport_to_source_y[(Y)] == map_viewport_to_source_y[(Y) - 1])
// This is to avoid flooring a number that is approximated to .9999999 and be explicit about it
#define FLOAT_TO_INT(x) ((int)((x) + 0.1f))

static const char *SETTING_BACKLIGHT = "DispBacklight";
/* 目标可在 target config.h 里覆盖这两个默认值（例：tab5 用 3x 整数缩放给触摸按键留 margin）。
 * custom_zoom 的上限同理：默认仍是 2.0，只有需要的 target 才放开。 */
#ifndef RG_DISPLAY_DEFAULT_SCALING
#define RG_DISPLAY_DEFAULT_SCALING RG_DISPLAY_SCALING_FIT
#endif
#ifndef RG_DISPLAY_DEFAULT_CUSTOM_ZOOM
#define RG_DISPLAY_DEFAULT_CUSTOM_ZOOM 1.0
#endif
#ifndef RG_DISPLAY_MAX_CUSTOM_ZOOM
#define RG_DISPLAY_MAX_CUSTOM_ZOOM 2.0
#endif

static const char *SETTING_SCALING = "DispScaling";
static const char *SETTING_FILTER = "DispFilter";
static const char *SETTING_ROTATION = "DispRotation";
static const char *SETTING_BORDER = "DispBorder";
static const char *SETTING_CUSTOM_ZOOM = "DispCustomZoom";

static void lcd_init(void);
static void lcd_deinit(void);
static void lcd_sync(void);
static void lcd_set_rotation(int rotation);
static void lcd_set_backlight(float percent);
static void lcd_set_window(int left, int top, int width, int height);
static inline uint16_t *lcd_get_buffer(size_t length);
static inline void lcd_send_buffer(uint16_t *buffer, size_t length);

#if RG_SCREEN_DRIVER == 0 /* ILI9341/ST7789 */
#include "drivers/display/ili9341.h"
#elif RG_SCREEN_DRIVER == 2 || RG_SCREEN_DRIVER == 3 || RG_SCREEN_DRIVER == 4
/* Tab5（含 0.4.9 单 app 双方向）：两份驱动同编，运行时按方向选一份。
 * 契约：下面所有 lcd_* 调用点一字不改 —— 分发层把它们定义成 static inline 转发器。
 * 单方向构建（双 app 形态 / 或单一方向的实验档）走的也是这里，结果与改造前逐字一致。 */
#include "drivers/display/tab5_dispatch.h"
#elif RG_SCREEN_DRIVER == 99
#include "drivers/display/sdl2.h"
#else
#include "drivers/display/dummy.h"
#endif

static inline unsigned blend_pixels(unsigned a, unsigned b)
{
    // Fast path (taken 80-90% of the time)
    if (a == b)
        return a;

    // Not the original author, but a good explanation is found at:
    // https://medium.com/@luc.trudeau/fast-averaging-of-high-color-16-bit-pixels-cb4ac7fd1488
    a = (a << 8) | (a >> 8);
    b = (b << 8) | (b >> 8);
    unsigned s = a ^ b;
    unsigned v = ((s & 0xF7DEU) >> 1) + (a & b) + (s & 0x0821U);
    return (v << 8) | (v >> 8);

    // This is my attempt at averaging two 565BE values without swapping bytes (3x the speed of the code above)
    // return (((a ^ b) & 0b1101111011110110U) >> 1) + (a & b);
}

static inline void write_update(const rg_surface_t *update)
{
    const int64_t time_start = rg_system_timer();

    bool filter_x = display.viewport.filter_x;
    bool filter_y = display.viewport.filter_y;
    int draw_left = display.viewport.left;
    int draw_top = display.viewport.top;
    int draw_width = display.viewport.width;
    int draw_height = display.viewport.height;

    int crop_left = 0;
    int crop_top = 0;

    if (draw_left < 0)
    {
        crop_left += -draw_left * display.viewport.step_x;
        draw_width += draw_left * 2;
        draw_left = 0;
    }

    if (draw_top < 0)
    {
        crop_top += -draw_top * display.viewport.step_y;
        draw_height += draw_top * 2;
        draw_top = 0;
    }

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* 触摸覆盖层（虚拟按键）里"外观变了的单元" → 交给**该画它的那条路**。
     *
     * 两种情形分开处理，判据是"本帧推送覆盖得到它吗"：
     *   · 覆盖得到（菜单：视口==整屏）→ 清零这些行的校验和，让本帧推送把它带上去
     *     （行校验和过滤会跳过源缓冲没变的行，而叠加层不在源缓冲里，不置脏就推不动）；
     *   · 覆盖不到（游戏：视口只有 720x480，控制条带在 y>=480 的黑边上，推帧循环根本
     *     遍历不到那些行）→ 在这里**重建那块条带的背景**（边框图或纯黑）再走一次正常
     *     发送路径 —— 驱动发送前会把叠加层合成上去，所以按压高亮/新标签一起出来。
     *
     * 这就是真机"点了不变、只有进出游戏才变"的根因和解法（进出游戏会触发整屏重画）。
     * 一句话：推帧循环到不了的地方，得有人负责画它。
     * ⚠ 全都在显示线程里做（本函数）。绝不要挪到输入任务去调 rg_display_force_redraw()：
     *   它会派发 RG_EVENT_REDRAW，启动器的 event_handler 收到就 gui_redraw() ——
     *   等于在输入任务里重画界面，两个线程抢同一块 surface（真机花屏，2026-09-29）。 */
    {
        int rects[RG_OVERLAY_DIRTY_MAX * 4];
        const int dirty_count = rg_overlay_take_dirty_rects(rects, RG_OVERLAY_DIRTY_MAX);
        bool any_outside = false;
        if (dirty_count)
            RG_LOGD("overlay dirty: %d rects, viewport %d,%d %dx%d\n",
                    dirty_count, draw_left, draw_top, draw_width, draw_height);
        for (int i = 0; i < dirty_count; ++i)
        {
            /* ⚠ 坐标口径：这些矩形来自触摸键位表 → 是**画布坐标**（横屏 1280x720，
             * 按键本来就落在可见区外的留白里）；而本函数往下（视口比较 / rg_display_clear_rect）
             * 一律用**可见区坐标**。所以先减 margins 归一，否则会像 2026-10-08 夜那样
             * 把 margins 加第二遍（真机：Bad lcd window (280,600,1280,240)）。
             * 竖屏 margins=0，此处恒等，行为逐字节不变。 */
            const int rx = rects[i * 4 + 0] - display.screen.margins.left;
            const int ry = rects[i * 4 + 1] - display.screen.margins.top;
            const int rw = rects[i * 4 + 2], rh = rects[i * 4 + 3];
            if (rx >= draw_left && ry >= draw_top &&
                rx + rw <= draw_left + draw_width && ry + rh <= draw_top + draw_height)
                continue;   /* 本帧推送会覆盖它 */
            any_outside = true;
            if (border)
            {
                /* 背景是边框图：把该矩形从边框图里搬过来（沿用 load_border_file 的画法） */
                const uint8_t *src = (const uint8_t *)border->data +
                                     (size_t)ry * border->stride + (size_t)rx * 2;
                rg_display_write_rect(rx, ry, rw, rh, border->stride, (const uint16_t *)src,
                                      RG_DISPLAY_WRITE_NOSYNC);
            }
            else
            {
                /* 背景是纯黑（视口外的填充色，见 rg_display_clear_except） */
                rg_display_clear_rect(rx, ry, rw, rh, C_BLACK);
            }
        }
        /* 全都在推送范围内 → 本帧的推送就会把它们画对（但要绕过行校验和过滤）。
         * 只置脏**这几块矩形覆盖的行**，不用整屏 —— 条带之外的行没变，重推是白推
         * （整屏重推在菜单里约 53ms，只置脏这几块降到 ~1/3）。 */
        if (dirty_count && !any_outside)
        {
            for (int i = 0; i < dirty_count; ++i)
            {
                const int ry = rects[i * 4 + 1], rh = rects[i * 4 + 3];
                const int top = RG_MAX(0, RG_MIN(ry, RG_SCREEN_HEIGHT - 1));
                const int lines = RG_MAX(0, RG_MIN(rh, RG_SCREEN_HEIGHT - top));
                if (lines)
                    memset(&screen_line_checksum[top], 0, (size_t)lines * sizeof(uint32_t));
            }
        }
    }
#endif

    const int format = update->format;
    const int stride = update->stride;
    const void *data = update->data + update->offset + (crop_top * stride) + (crop_left * RG_PIXEL_GET_SIZE(format));
    const uint16_t *palette = update->palette;

    const bool partial_update = RG_SCREEN_PARTIAL_UPDATES;
    /* P2-2：只提交变化行（SD 卡开关）。关闭时下面的推送逻辑与改动前**逐字节等价**，
     * 所以这个开关本身可以安全地留在发行版里。 */
    const bool dirty_rows = partial_update && rg_display_dirty_rows_on();

    int lines_per_buffer = LCD_BUFFER_LENGTH / draw_width;
    if (dirty_rows && lines_per_buffer > RG_DIRTY_ROW_MAX)
        lines_per_buffer = RG_DIRTY_ROW_MAX;   /* 掩码数组上限（见 RG_DIRTY_ROW_MAX） */
    int lines_remaining = draw_height;
    int lines_updated = 0;
    int window_top = -1;

    for (int y = 0; y < draw_height;)
    {
        int lines_to_copy = RG_MIN(lines_per_buffer, lines_remaining);

        if (lines_to_copy < 1)
            break;

        // The vertical filter requires a block to start and end with unscaled lines
        if (filter_y)
        {
            while (lines_to_copy > 1 && (LINE_IS_REPEATED(y + lines_to_copy - 1) ||
                                         LINE_IS_REPEATED(y + lines_to_copy)))
                --lines_to_copy;
        }

        uint16_t *line_buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
        uint16_t *line_buffer_ptr = line_buffer;

        uint32_t checksum = 0xFFFFFFFF;
        bool need_update = !partial_update;
        /* P2-2：本块内每行"内容是否变了"的掩码（只在开关打开时维护，关闭时零成本） */
        uint8_t line_changed[RG_DIRTY_ROW_MAX];
        if (dirty_rows)
            memset(line_changed, 0, sizeof(line_changed));

        for (int i = 0; i < lines_to_copy; ++i)
        {
            if (i > 0 && LINE_IS_REPEATED(y))
            {
                memcpy(line_buffer_ptr, line_buffer_ptr - draw_width, draw_width * 2);
                line_buffer_ptr += draw_width;
            }
            else
            {
                #define RENDER_LINE(PTR_TYPE, PIXEL) { \
                    PTR_TYPE *buffer = (PTR_TYPE *)(data + map_viewport_to_source_y[y] * stride);\
                    for (int xx = 0; xx < draw_width; ++xx) { \
                        int x = map_viewport_to_source_x[xx]; \
                        *line_buffer_ptr++ = (PIXEL); \
                    } \
                }
                if (format & RG_PIXEL_PALETTE)
                    RENDER_LINE(uint8_t, palette[buffer[x]])
                else if (format == RG_PIXEL_565_LE)
                    RENDER_LINE(uint16_t, (buffer[x] << 8) | (buffer[x] >> 8))
                else
                    RENDER_LINE(uint16_t, buffer[x])

                if (partial_update)
                {
                    checksum = rg_hash((void*)(line_buffer_ptr - draw_width), draw_width * 2);
                }
            }

            if (screen_line_checksum[draw_top + y] != checksum)
            {
                screen_line_checksum[draw_top + y] = checksum;
                need_update = true;
                if (dirty_rows)
                    line_changed[i] = 1;   /* P2-2：记下"这一行变了" */
#if defined(RG_GBA_DIAG) && RG_GBA_DIAG
                rg_display_dirty_lines++;
#endif
            }

            ++y;
        }

        if (filter_x && need_update)
        {
            for (int i = 0; i < lines_to_copy; ++i)
            {
                uint16_t *buffer = line_buffer + i * draw_width;
                for (int x = 1; x < draw_width - 1; ++x)
                {
                    if (map_viewport_to_source_x[x] == map_viewport_to_source_x[x - 1])
                    {
                        buffer[x] = blend_pixels(buffer[x - 1], buffer[x + 1]);
                    }
                }
            }
        }

        if (filter_y && need_update)
        {
            int top = y - lines_to_copy;
            for (int i = 1; i < lines_to_copy - 1; ++i)
            {
                if (LINE_IS_REPEATED(top + i))
                {
                    uint16_t *lineA = line_buffer + (i - 1) * draw_width;
                    uint16_t *lineB = line_buffer + (i + 0) * draw_width;
                    uint16_t *lineC = line_buffer + (i + 1) * draw_width;
                    for (size_t x = 0; x < draw_width; ++x)
                    {
                        lineB[x] = blend_pixels(lineA[x], lineC[x]);
                    }
                }
            }
        }

        if (need_update)
        {
            int left = display.screen.margins.left + draw_left;
            int top = display.screen.margins.top + draw_top + y - lines_to_copy;
            int dirty_n = 0;
            if (dirty_rows)
                for (int i = 0; i < lines_to_copy; ++i)
                    dirty_n += line_changed[i] ? 1 : 0;

            if (dirty_rows && dirty_n < lines_to_copy)
            {
                /* ── P2-2：只推"变化行的连续段"─────────────────────────────────────
                 * 块内整块都变时走下面的原路径（窗口按剩余高度设一次、后续块流式追加），
                 * 这里只处理"部分行没变"的情况。
                 * ⚠ 膨胀 1 行：垂直滤波用 i±1 混合出重复行，漏推邻居会留残影（见文件头）。
                 *   就地膨胀，先左→右再右→左各扫一遍即可。 */
                if (filter_y)
                {
                    for (int i = 1; i < lines_to_copy; ++i)
                        if (line_changed[i - 1])
                            line_changed[i] = 1;
                    for (int i = lines_to_copy - 2; i >= 0; --i)
                        if (line_changed[i + 1])
                            line_changed[i] = 1;
                }
                int run = -1;
                for (int i = 0; i <= lines_to_copy; ++i)
                {
                    const bool changed = (i < lines_to_copy) && line_changed[i];
                    if (changed && run < 0)
                    {
                        run = i;
                    }
                    else if (!changed && run >= 0)
                    {
                        const int n = i - run;
                        lcd_set_window(left, top + run, draw_width, n);
                        lcd_send_buffer(line_buffer + (size_t)run * draw_width, draw_width * n);
                        lines_updated += n;
                        run = -1;
                    }
                }
                /* 窗口已被逐段改写过，作废流式记账，别让下一块误复用窗口 */
                window_top = -1;
            }
            else
            {
                if (top != window_top)
                    lcd_set_window(left, top, draw_width, lines_remaining);
                lcd_send_buffer(line_buffer, draw_width * lines_to_copy);
                window_top = top + lines_to_copy;
                lines_updated += lines_to_copy;
            }
        }
        else
        {
            // Return unused buffer
            lcd_send_buffer(line_buffer, 0);
        }

        lines_remaining -= lines_to_copy;
    }

    if (osd != NULL)
    {
        // TODO: Draw on screen display. By default it should be bottom left which is fine
        // for both virtual keyboard and info labels. Maybe make it configurable later...
    }

    if (lines_updated > draw_height * 0.80f)
        counters.fullFrames++;
    else
        counters.partFrames++;
    counters.busyTime += rg_system_timer() - time_start;
}

static void update_viewport_scaling(void)
{
    int screen_width = display.screen.width;
    int screen_height = display.screen.height;
    int src_width = display.source.width;
    int src_height = display.source.height;
    int new_width = src_width;
    int new_height = src_height;

    if (config.scaling == RG_DISPLAY_SCALING_FULL)
    {
        new_width = screen_width;
        new_height = screen_height;
    }
    else if (config.scaling == RG_DISPLAY_SCALING_FIT)
    {
        new_width = FLOAT_TO_INT(screen_height * ((float)src_width / src_height));
        new_height = screen_height;
        if (new_width > screen_width) {
            new_width = screen_width;
            new_height = FLOAT_TO_INT(screen_width * ((float)src_height / src_width));
        }
    }
    else if (config.scaling == RG_DISPLAY_SCALING_ZOOM)
    {
        /* 规则：**尽量取最大的整数放大倍数**，唯一约束是放得下。两道闸：
         *   ① 宽度不超逻辑屏宽（720）—— 超了会被居中成负 left（切边），宁可小一点。
         *   ② 高度不超 RG_DISPLAY_MAX_WINDOW_HEIGHT（620 = 物理屏 1280 − 控制区 660）——
         *      画面下面必须留得下触摸按键。
         * 于是各机型的倍数是算出来的：GB/GBC 4x(640x576)、GBA 3x(720x480)、NES 2x(512x480)。
         * ⚠ 别拿"逻辑屏高"当高度上限：那是 480（GBA 满宽画面的高度），控制区长在物理屏
         *   1280 上，用 480 去算会把 GB 4x 一路砍到 1x。
         * ⚠ custom_zoom（默认 4.0 = Tab5 上限）只作为**上限的进一步收紧**，用户调小才生效。 */
#ifndef RG_DISPLAY_MAX_WINDOW_HEIGHT
#define RG_DISPLAY_MAX_WINDOW_HEIGHT screen_height   /* 未定义的 target = 沿用逻辑屏高 */
#endif
        const int max_zoom_w = screen_width / src_width;
        const int max_zoom_h = RG_DISPLAY_MAX_WINDOW_HEIGHT / src_height;
        int max_zoom = RG_MIN(max_zoom_w, max_zoom_h);
        if (max_zoom < 1)
            max_zoom = 1;
        int zoom = FLOAT_TO_INT(config.custom_zoom);
        if (zoom > max_zoom)
            zoom = max_zoom;
        if (zoom < 1)
            zoom = 1;
        new_width = src_width * zoom;
        new_height = src_height * zoom;
        RG_LOGI("display: zoom source %dx%d requested x%g → x%d (%dx%d), max x%d, ctrl left %d\n",
                src_width, src_height, config.custom_zoom, zoom, new_width, new_height,
                max_zoom, RG_SCREEN_HEIGHT - new_height);
    }

    // Everything works better when we use even dimensions!
    new_width &= ~1;
    new_height &= ~1;

    display.viewport.left = (screen_width - new_width) / 2;
    /* 太高就**贴顶**：Tab5 的游戏区只有 480 高（见 targets/tab5/config.h 的
     * RG_SCREEN_VISIBLE_AREA），GB 4x = 576 高会算出 top = (480-576)/2 = **-48**
     * （负值 → 画面顶部被切）。贴顶后画面占 y 0..576，控制区从 576 起。
     * GBA(480)/NES(480) 本来就不超 → 结果为 0，与改前完全相同。 */
    display.viewport.top = RG_MAX(0, (screen_height - new_height) / 2);
    display.viewport.width = new_width;
    display.viewport.height = new_height;

    display.viewport.step_x = (float)src_width / display.viewport.width;
    display.viewport.step_y = (float)src_height / display.viewport.height;

    display.viewport.filter_x = (config.filter == RG_DISPLAY_FILTER_HORIZ || config.filter == RG_DISPLAY_FILTER_BOTH) &&
                                (config.scaling && (display.viewport.width % src_width) != 0);
    display.viewport.filter_y = (config.filter == RG_DISPLAY_FILTER_VERT || config.filter == RG_DISPLAY_FILTER_BOTH) &&
                                (config.scaling && (display.viewport.height % src_height) != 0);

    memset(screen_line_checksum, 0, sizeof(screen_line_checksum));

    /* 映射表按**视口**填（不是可见区）：视口可能比可见区还高（GB 4x = 576 > 480），
     * 只填到 screen_height 的话，480..575 那些行会残留上一次的映射（画面下半截拉错）。
     * 数组本身按 RG_SCREEN_WIDTH/HEIGHT 分配，容量足够。 */
    for (int x = 0; x < display.viewport.width; ++x)
        map_viewport_to_source_x[x] = FLOAT_TO_INT(x * display.viewport.step_x);
    for (int y = 0; y < display.viewport.height; ++y)
        map_viewport_to_source_y[y] = FLOAT_TO_INT(y * display.viewport.step_y);

    RG_LOGI("%dx%d@%.3f => %dx%d@%.3f left:%d top:%d step_x:%.2f step_y:%.2f", src_width, src_height,
            (float)src_width / src_height, new_width, new_height, (float)new_width / new_height,
            display.viewport.left, display.viewport.top, display.viewport.step_x, display.viewport.step_y);
}

static bool load_border_file(const char *filename)
{
    RG_LOGI("Loading border file: %s", filename ?: "(none)");

    /* 内存面板（皮肤底图）不归我们分配：只脱手，别 free —— 所有权在 rg_touch_skin.c，
     * free 掉会让那边的 panel 变成野指针（下次换皮肤原地重画就踩空）。 */
    if (border && !border_foreign)
        free(border);
    border = NULL;
    border_foreign = false;
    display.changed = true;

    if (filename && (border = rg_surface_load_image_file(filename, 0)))
    {
        if (border->width != rg_display_get_width() || border->height != rg_display_get_height())
        {
            rg_surface_t *resized = rg_surface_resize(border, rg_display_get_width(), rg_display_get_height());
            if (resized)
            {
                rg_surface_free(border);
                border = resized;
            }
        }
        return true;
    }
    return false;
}

IRAM_ATTR
static void display_task(void *arg)
{
    rg_task_msg_t msg;

    while (rg_task_peek(&msg))
    {
        // Received a shutdown request!
        if (msg.type == RG_TASK_MSG_STOP)
            break;

        if (display.changed)
        {
            update_viewport_scaling();
            // Clear the screen if the viewport doesn't cover the entire screen because garbage could remain on the sides
            if (display.viewport.width < display.screen.width || display.viewport.height < display.screen.height)
            {
                if (border)
                    rg_display_write_rect(0, 0, border->width, border->height, 0, border->data, RG_DISPLAY_WRITE_NOSYNC);
                else
                    rg_display_clear_except(display.viewport.left, display.viewport.top, display.viewport.width, display.viewport.height, C_BLACK);
            }
            display.changed = false;
        }

        write_update(msg.dataPtr);

        rg_task_receive(&msg);

        lcd_sync();
    }
}

void rg_display_force_redraw(void)
{
    display.changed = true;
    // memset(screen_line_checksum, 0, sizeof(screen_line_checksum));
    rg_system_event(RG_EVENT_REDRAW, NULL);
    rg_display_sync(true);
}

const rg_display_t *rg_display_get_info(void)
{
    return &display;
}

rg_display_counters_t rg_display_get_counters(void)
{
    return counters;
}

int rg_display_get_width(void)
{
    // return display.screen.real_width - (display.screen.margins.left + display.screen.margins.right);
    return display.screen.width;
}

int rg_display_get_height(void)
{
    // return display.screen.real_height - (display.screen.margins.top + display.screen.margins.bottom);
    return display.screen.height;
}

void rg_display_set_scaling(display_scaling_t scaling)
{
    config.scaling = RG_MIN(RG_MAX(0, scaling), RG_DISPLAY_SCALING_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_SCALING, config.scaling);
    display.changed = true;
}

display_scaling_t rg_display_get_scaling(void)
{
    return config.scaling;
}

void rg_display_set_custom_zoom(double factor)
{
    config.custom_zoom = RG_MIN(RG_MAX(0.1, factor), RG_DISPLAY_MAX_CUSTOM_ZOOM);
    rg_settings_set_number(NS_APP, SETTING_CUSTOM_ZOOM, config.custom_zoom);
    display.changed = true;
}

double rg_display_get_custom_zoom(void)
{
    return config.custom_zoom;
}

/* 按机型切换"画面可用区"：横屏下"没有肩键行"的机型（GB/GBC/NES/GG/SMS/COL/PCE/GW/Lynx）
 * 把上边界抬到 0、下边界放到 576 —— 顶部那条本来就没键，让 4x 画面（640×576）吃满它；
 * 有肩键行的机型（GBA/SNES/菜单）沿用 {280,120,280,120}（顶部留给 L/R + L/R 调换键）。
 * 谁调用：retro-core/main/main.c 的 app_main()（每个核心二进制都会经过），紧随 rg_system_init()。
 * ⚠ 时机：必须在核心第一帧之前 —— 视口（display.viewport）是在那时按可见区算出来的。
 * 竖屏两个档位同值 → 本函数恒等，行为与改动前逐字节一致。 */
void rg_display_set_visible_area_for_console(const char *console_id)
{
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* 两档都是 4 个 int，用数组选档（margins 是匿名结构字段，没法整体赋值） */
    const int with_shld[4]    = RG_SCREEN_VISIBLE_AREA;   /* 0.4.9：可见区随方向变 ⇒ 不能 static */
    const int without_shld[4] = RG_SCREEN_VISIBLE_AREA_NO_SHLD;   /* 0.4.9：可见区随方向变 ⇒ 不能 static */
    const int *m = rg_touch_has_shoulders(console_id) ? with_shld : without_shld;

    if (display.screen.margins.left == m[0] && display.screen.margins.top == m[1] &&
        display.screen.margins.right == m[2] && display.screen.margins.bottom == m[3])
        return;     /* 已经是这一档，别白重算一遍视口 */

    RG_LOGI("visible area: console '%s' → margins %d,%d,%d,%d (was %d,%d,%d,%d)\n",
            console_id ? console_id : "?", m[0], m[1], m[2], m[3],
            display.screen.margins.left, display.screen.margins.top,
            display.screen.margins.right, display.screen.margins.bottom);

    display.screen.margins.left = m[0];
    display.screen.margins.top = m[1];
    display.screen.margins.right = m[2];
    display.screen.margins.bottom = m[3];
    display.screen.width  = display.screen.real_width  - (m[0] + m[2]);
    display.screen.height = display.screen.real_height - (m[1] + m[3]);
    display.changed = true;     /* 让显示任务重算视口（与 set_scaling 同一个触发器） */
#else
    (void)console_id;           /* 无触摸叠加层 = 只有一档，无需切换 */
#endif
}

void rg_display_set_filter(display_filter_t filter)
{
    config.filter = RG_MIN(RG_MAX(0, filter), RG_DISPLAY_FILTER_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_FILTER, config.filter);
    display.changed = true;
}

display_filter_t rg_display_get_filter(void)
{
    return config.filter;
}

void rg_display_set_rotation(display_rotation_t rotation)
{
    config.rotation = RG_MIN(RG_MAX(0, rotation), RG_DISPLAY_ROTATION_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_SCALING, config.rotation);
    display.changed = true;
}

display_rotation_t rg_display_get_rotation(void)
{
    return config.rotation;
}

void rg_display_set_backlight(display_backlight_t percent)
{
    config.backlight = RG_MIN(RG_MAX(percent, RG_DISPLAY_BACKLIGHT_MIN), RG_DISPLAY_BACKLIGHT_MAX);
    rg_settings_set_number(NS_GLOBAL, SETTING_BACKLIGHT, config.backlight);
    lcd_set_backlight(config.backlight);
}

display_backlight_t rg_display_get_backlight(void)
{
    return config.backlight;
}

void rg_display_set_border(const char *filename)
{
    free(config.border_file);
    config.border_file = NULL;

    if (load_border_file(filename))
    {
        rg_settings_set_string(NS_APP, SETTING_BORDER, filename);
        config.border_file = strdup(filename);
    }
    else
    {
        rg_settings_set_string(NS_APP, SETTING_BORDER, NULL);
        config.border_file = NULL;
    }
    display.changed = true;
}

char *rg_display_get_border(void)
{
    return rg_settings_get_string(NS_APP, SETTING_BORDER, NULL);
}

/* 内存面板当边框用（皮肤底图）：surface **由调用方持有**，本模块只借指针，永不 free。
 * 为什么复用 border 这条路：脏条带重建（画面外围的重绘）本来就从 border 取背景 ——
 * 面板接进来，"控制区底图"这件事一行新机制都不用加。
 * ⚠ 只在用户没手选 Border 图时生效（用户的选择优先）。 */
void rg_display_set_border_surface(rg_surface_t *surface)
{
    /* ⚠ 横屏（画布 1280x720）：皮肤的"面板底图"**内容仍是竖屏布局**画的 ——
     * 分区框 / 凹槽 / "画面-控制区分界线"全按"画面上、控制区下"的竖屏关系算，
     * 塞进横屏画布后散成几根无来源的细线、短刻度（用户 2026-10-08 真机照片原话：
     * "皮肤的边框都乱了"；拍照可见：START/MENU 之间一条细横线、画面左侧一条窄竖带带两短刻度）。
     * 这块艺术要真正横屏化 = 一次 GUI 改动（按用户规矩须**先出图确认**），所以今晚先**不接管**：
     * 画面干净，按键/铭牌照常显示（它们走叠加层，与本底图无关）。
     * 回退方式：删掉下面这个分支即可恢复接管。 */
    if (RG_SCREEN_WIDTH > RG_SCREEN_HEIGHT)
    {
        RG_LOGW("display: 横屏下暂不接管内存面板底图（皮肤底图尚未横屏化），保持画面干净\n");
        return;
    }
    if (config.border_file)
    {
        RG_LOGI("display: 用户已选边框图 (%s)，内存面板不接管\n", config.border_file);
        return;
    }
    /* 尺寸校验：面板是**物理全屏**底图（Tab5 竖屏 720x1280），而 rg_display_get_width/height
     * 报的是**逻辑视口**（Tab5 上游戏视口只有 720x480）——拿后者做"必须相等"会把面板全拒掉
     * （真机日志："内存面板尺寸不符（720x1280，应为 720x480），忽略"）。
     * 改成"**必须盖住逻辑视口**"：够大才可能当底图，小了确实该拒。 */
    if (!surface || surface->width < rg_display_get_width() || surface->height < rg_display_get_height())
    {
        RG_LOGW("display: 内存面板尺寸不足（%dx%d，逻辑视口需 %dx%d），忽略\n",
                surface ? surface->width : 0, surface ? surface->height : 0,
                rg_display_get_width(), rg_display_get_height());
        return;
    }

    border = surface;
    border_foreign = true;
    display.changed = true;     /* 整屏重推一次：面板内容变了 */
    RG_LOGI("display: 内存面板接管边框底图 (%dx%d)\n", surface->width, surface->height);
}

/* 面板内容已**原地**改过（换皮肤/换机型）：让显示任务把整张边框重铺一遍。
 * 走 display.changed 这条现成通路 —— 视口不覆盖整屏时会整张重写（GBA 竖屏正是如此）。 */
void rg_display_border_refresh(void)
{
    display.changed = true;
}

void rg_display_submit(const rg_surface_t *update, uint32_t flags)
{
    const int64_t time_start = rg_system_timer();

    // Those things should probably be asserted, but this is a new system let's be forgiving...
    if (!update || !update->data)
        return;

    if (display.source.width != update->width || display.source.height != update->height)
    {
        rg_display_sync(true);
        display.source.width = update->width;
        display.source.height = update->height;
        display.changed = true;
    }

    rg_task_send(display_task_queue, &(rg_task_msg_t){.dataPtr = update});

    counters.blockTime += rg_system_timer() - time_start;
    counters.totalFrames++;
}

bool rg_display_sync(bool block)
{
    /* 2026-09-28 性能盲区仪表：模拟器线程**等显示**的阻塞时间。
     * 驱动侧的 display= 量的是显示任务自己的工作量；这里量的是调用方被拖住多久。
     * 两者相加才是显示对帧时间的真实侵占。 */
    int64_t pf_t0 = rg_system_timer();
    /* 丢块体检（2026-10-03）：每 ~10 秒，只在丢块数有变化时报一次。
     * 有 drops 但 redirty 跟着涨 = 修复在生效（这些行被退回重推，不会定格）；
     * 修复前同样的 drops 就是"静态画面光标卡死"的直接来源。 */
    {
        static uint32_t last_drops = 0;
        static uint32_t tick = 0;
        if ((++tick % 600) == 0 && rg_display_push_drops != last_drops)
        {
            RG_LOGI("display: drops=%u redirty_rows=%u (每 10s 增量 %u)\n",
                    (unsigned)rg_display_push_drops, (unsigned)rg_display_push_redirty,
                    (unsigned)(rg_display_push_drops - last_drops));
            last_drops = rg_display_push_drops;
        }
    }

    while (block && rg_task_messages_waiting(display_task_queue))
    {
#if defined(RG_TARGET_SDL2)
        rg_task_delay(1); // 宿主上忙等会空烧 CPU（这里在主线程，delay 顺带泵事件）
#else
        rg_task_yield(); // 真机：让出 CPU 给显示任务，不要睡一个 tick
#endif
    }
    {
        static int64_t pf_win = 0, pf_wait = 0;
        static uint32_t pf_calls = 0;
        int64_t now = rg_system_timer();
        pf_wait += now - pf_t0;
        pf_calls++;
        if (pf_win == 0) pf_win = now;
        if (now - pf_win >= 1000000) {
            /* 探针已停用（性能排查期临时加入，疑与游戏内改音量崩溃相关） */
    /* RG_LOGI("SYNC-PF: 等显示=%d us/秒  calls=%d\n", (int)pf_wait, (int)pf_calls); */
            pf_win = now; pf_wait = 0; pf_calls = 0;
        }
    }
    return !rg_task_messages_waiting(display_task_queue);
}

/* 强制某些逻辑行在下一帧重推（把校验和清零）。
 * 用途：屏幕上的帧率数字每秒都在变，但它所在区域平时不在脏区里，
 * 不主动置脏就永远不重画 —— 真机表现为"帧率数字不刷新，只有点开 menu 才更新"。
 * 这是本文件既有的"校验和置零强制重推"手法（见 rg_display_write_rect）。 */
void rg_display_invalidate_lines(int top, int count)
{
    if (top < 0) { count += top; top = 0; }
    for (int y = 0; y < count; ++y)
        if (top + y < display.screen.height)
            screen_line_checksum[top + y] = 0;
}

void rg_display_write_rect(int left, int top, int width, int height, int stride, const uint16_t *buffer, uint32_t flags)
{
    RG_ASSERT_ARG(buffer);

    // calc stride before clipping width
    stride = RG_MAX(stride, width * 2);

    // Clipping
    /* ⚠ 基准是**物理屏**（real_*），不是逻辑视口（width/height）：
     *   Tab5 竖屏上逻辑视口只有 720x480，而控制区条带（物理 y 480..1280）和边框图**都在它之外**。
     *   用逻辑尺寸裁，`height = MIN(h, 480 - 480) = 0` → 一个像素都不写。
     *   真机症状极具迷惑性：面板装上了、日志条条都对，控制区永远纯黑，只有"整帧发送"时才见按键变化
     *   （覆盖层是驱动合成进物理帧缓冲的，不经过这里）。2026-10-07 定位。
     *   screen_line_checksum[] 是按 real_height 开的，所以这里用 real 不会越界。 */
    width = RG_MIN(width, display.screen.real_width - left);
    height = RG_MIN(height, display.screen.real_height - top);

    // This can happen when left or top is out of bound
    if (width < 0 || height < 0)
        return;

    // This will work for now because we rarely draw from different threads (so all we need is ensure
    // that we're not interrupting a display update). But what we SHOULD be doing is acquire a lock
    // before every call to lcd_set_window and release it only after the last call to lcd_send_buffer.
    if (!(flags & RG_DISPLAY_WRITE_NOSYNC))
        rg_display_sync(true);

    // This isn't really necessary but it makes sense to invalidate
    // the lines we're about to overwrite...
    for (size_t y = 0; y < (size_t)height; ++y)
        if ((size_t)(top + y) < RG_COUNT(screen_line_checksum))   // 物理坐标可能到 1279，别越界
            screen_line_checksum[top + y] = 0;

    lcd_set_window(left + display.screen.margins.left, top + display.screen.margins.top, width, height);

    for (size_t y = 0; y < height;)
    {
        uint16_t *lcd_buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
        size_t num_lines = RG_MIN(LCD_BUFFER_LENGTH / width, height - y);

        // Copy line by line because stride may not match width
        for (size_t line = 0; line < num_lines; ++line)
        {
            uint16_t *src = (void *)buffer + ((y + line) * stride);
            uint16_t *dst = lcd_buffer + (line * width);
            if (flags & RG_DISPLAY_WRITE_NOSWAP)
            {
                memcpy(dst, src, width * 2);
            }
            else
            {
                for (size_t i = 0; i < width; ++i)
                    dst[i] = (src[i] >> 8) | (src[i] << 8);
            }
        }

        lcd_send_buffer(lcd_buffer, width * num_lines);
        y += num_lines;
    }

    lcd_sync();
}

void rg_display_clear_rect(int left, int top, int width, int height, uint16_t color_le)
{
    const uint16_t color_be = (color_le << 8) | (color_le >> 8);

    /* 负坐标 / 越界矩形裁剪 —— 与 rg_display_write_rect 同款防御（上游 write_rect 一直有，
     * clear_rect 一直没有）。不裁的话 lcd_set_window 会收到负的窗口，真机上只打一行
     * "Bad lcd window" 然后照旧按错坐标推送（且像素数按未裁的算）。GBA/SNES 清黑框时会传出
     * 负坐标；losoco/retro-go-majula-pulic 也在同一处补了同样的裁剪（2026-10-08 对照）。
     *
     * ⚠ 顺序：**先加 margins 再裁剪**。rg_display_clear() 就是靠 left = -margins.left 把窗口
     *   拉到 (0,0) 的，提前裁剪会把整屏清屏切错位（Tab5 上 margins.left/top 都是 0，
     *   所以这条目前只在别的 target 上起作用，但契约必须写对）。 */
    left += display.screen.margins.left;
    top += display.screen.margins.top;
    if (left < 0)
    {
        width += left;
        left = 0;
    }
    if (top < 0)
    {
        height += top;
        top = 0;
    }
    if (left >= display.screen.real_width || top >= display.screen.real_height)
        return;
    width = RG_MIN(width, display.screen.real_width - left);
    height = RG_MIN(height, display.screen.real_height - top);
    if (width <= 0 || height <= 0)
        return;

    int pixels_remaining = width * height;
    if (pixels_remaining > 0)
    {
        lcd_set_window(left, top, width, height);
        while (pixels_remaining > 0)
        {
            uint16_t *buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
            int pixels = RG_MIN(pixels_remaining, LCD_BUFFER_LENGTH);
            for (size_t j = 0; j < pixels; ++j)
                buffer[j] = color_be;
            lcd_send_buffer(buffer, pixels);
            pixels_remaining -= pixels;
        }
    }
}

void rg_display_clear_except(int left, int top, int width, int height, uint16_t color_le)
{
    // Clear everything except the specified area
    // FIXME: Do not ignore left/top...
    int left_offset = -display.screen.margins.left;
    int top_offset = -display.screen.margins.top;
    int horiz = (display.screen.real_width - width + 1) / 2;
    int vert = (display.screen.real_height - height + 1) / 2;
    rg_display_clear_rect(left_offset, top_offset, horiz, display.screen.real_height, color_le); // Left
    rg_display_clear_rect(left_offset + horiz + width, top_offset, horiz, display.screen.real_height, color_le); // Right
    rg_display_clear_rect(left_offset + horiz, top_offset, display.screen.real_width - horiz * 2, vert, color_le); // Top
    rg_display_clear_rect(left_offset + horiz, top_offset + vert + height, display.screen.real_width - horiz * 2, vert, color_le); // Bottom
}

void rg_display_clear(uint16_t color_le)
{
    // We ignore margins here, we want to fill the entire screen
    rg_display_clear_rect(-display.screen.margins.left, -display.screen.margins.top, display.screen.real_width,
                          display.screen.real_height, color_le);
}

void rg_display_deinit(void)
{
    rg_task_send(display_task_queue, &(rg_task_msg_t){.type = RG_TASK_MSG_STOP});
    // lcd_set_backlight(0);
    lcd_deinit();
    RG_LOGI("Display terminated.\n");
}

void rg_display_init(void)
{
    RG_LOGI("Initialization...\n");
    // TO DO: We probably should call the setters to ensure valid values...
    config = (rg_display_config_t){
        .backlight = rg_settings_get_number(NS_GLOBAL, SETTING_BACKLIGHT, 80),
        .scaling = rg_settings_get_number(NS_APP, SETTING_SCALING, RG_DISPLAY_DEFAULT_SCALING),
        .filter = rg_settings_get_number(NS_APP, SETTING_FILTER, RG_DISPLAY_FILTER_BOTH),
        .rotation = rg_settings_get_number(NS_APP, SETTING_ROTATION, RG_DISPLAY_ROTATION_AUTO),
        .border_file = rg_settings_get_string(NS_APP, SETTING_BORDER, NULL),
        .custom_zoom = rg_settings_get_number(NS_APP, SETTING_CUSTOM_ZOOM, RG_DISPLAY_DEFAULT_CUSTOM_ZOOM),
    };
    /* 【锁定缩放】忽略 NVS 里存过的 Scaling/Factor —— 按**当前方向**判断（横屏锁、竖屏不锁）。
     * 横屏下"按键不压画面"完全依赖 ZOOM：用户可能在上一个方向（竖屏）存过 FULL 或别的倍数，
     * 切过来若不强制回默认，画面就会被拉满、按键压到画面上。
     * 0.4.9：同一个镜像要装两个方向 ⇒ 这里是**运行时**判断（原来靠 #if 编译期分流）。 */
    if (rg_geom()->lock_scaling)
    {
        config.scaling = RG_DISPLAY_DEFAULT_SCALING;
        config.custom_zoom = RG_DISPLAY_DEFAULT_CUSTOM_ZOOM;
    }
    display = (rg_display_t){
        .screen.real_width = RG_SCREEN_WIDTH,
        .screen.real_height = RG_SCREEN_HEIGHT,
        .screen.width = RG_SCREEN_WIDTH,
        .screen.height = RG_SCREEN_HEIGHT,
        .screen.margins = RG_SCREEN_VISIBLE_AREA,
        .changed = true,
    };
    display.screen.width -= display.screen.margins.left + display.screen.margins.right;
    display.screen.height -= display.screen.margins.top + display.screen.margins.bottom;
    lcd_init();
    rg_display_clear(C_BLACK);
    rg_task_delay(80); // Wait for the screen be cleared before turning on the backlight (40ms doesn't seem to be enough...)
    lcd_set_backlight(config.backlight);
    display_task_queue = rg_task_create_ex("rg_display", &display_task, NULL, 4 * 1024, RG_TASK_PRIORITY_6, 1, RG_DISPLAY_QUEUE_LEN);
    if (config.border_file)
        load_border_file(config.border_file);
    RG_LOGI("Display ready.\n");
}
