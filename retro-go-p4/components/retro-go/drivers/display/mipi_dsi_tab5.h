/* ============================================================================
 * M5Stack Tab5 (ESP32-P4) 显示驱动 — MIPI DSI / ST7123，走官方 Tab5 BSP
 * ----------------------------------------------------------------------------
 * 面板原生 = 720x1280 竖屏（真机实测：把 DPI 流切成 1280x720 + MADCTL MV(0x23)
 * 会黑屏 —— 背光亮但面板锁不住信号；M5 两家官方代码也都用竖屏时序 + 上层旋转）。
 * 逻辑画面 = 1280x720 横向，本驱动按"顺时针 90°"映射写入竖屏 FB：
 *     逻辑 (lx, ly)  ->  物理 (px, py) = (719 - ly, lx)
 * 映射方向已用四角锚点图案在真机目视确认（git tag v0.2）。
 *
 * 为什么显示驱动自己不做后续放大：P3 的 240x160 -> 3x 放大与旋转，改用 P4 的
 * PPA 硬件 SRM（旋转+缩放一次完成，零 CPU），届时替换这里的推送实现即可。
 *
 * 关键坑（技能条目）：
 *  - 不要在 lcd_init 里手写 esp_lcd_new_panel_dpi + DBI init 命令：
 *    P4 rev1.3 上会挂 task WDT（7/13 那轮的根因）。全部交给 Tab5 BSP。
 *  - 背光必须在任何 set 之前初始化（8.31），且先置 0 避免白闪。
 *  - 必须调用 RG_SCREEN_INIT()（本 target 里是空宏，作为显式约定钩子；
 *    漏掉它的历史教训是"显示驱动永不执行 = 黑屏"）。
 *  - rg_display.c 对 RG_PIXEL_565_LE 源已做一次交换 -> 到这里是 565 大端；
 *    DSI 面板要小端，所以逐像素换回。
 * ==========================================================================*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"    /* esp_lcd_dpi_panel_get_frame_buffer：PPA 要直接写帧缓冲 */
#include "driver/ppa.h"          /* PPA SRM 硬件旋转：替代 CPU 转置，省掉 cache 写回 */
#include "esp_cache.h"           /* esp_cache_msync：按键叠加层写帧缓冲后的失效/写回 */
#include "esp_timer.h"           /* 性能打点：区分"显示路径"与"模拟器核心"的 CPU 占用 */
#include "bsp/display.h"
#include "driver/i2c_master.h"   /* IO 扩展器 PI4IOE 的 API 需要 i2c_master_bus_handle_t */
#include "rg_input.h"            /* 虚拟按键可视层要读键位表 */
/* 注意：不要 #include "bsp/m5stack_tab5.h" —— 它的 umbrella 头会拉 lvgl.h
 * （retro-go 不编 LVGL）。需要什么就手写 extern，见下。 */

/* BSP 里这几个函数的声明被关在头文件的 LVGL 段内（BSP_CONFIG_NO_GRAPHIC_LIB=1
 * 把它们编译掉了），所以照 snowveil 的做法手写 extern 声明。 */
extern esp_err_t bsp_display_new_with_handles(
    const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles);
extern esp_err_t bsp_display_new_with_handles_to_st7123(
    const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles);

#define TAB5_PHYS_W 720    /* 面板物理（竖屏）宽 */
#define TAB5_PHYS_H 1280   /* 面板物理（竖屏）高 */

static esp_lcd_panel_handle_t tab5_panel = NULL;
static uint16_t tab5_line_buffer[LCD_BUFFER_LENGTH];   /* retro-go 收集逻辑行用 */
static uint16_t tab5_scratch[LCD_BUFFER_LENGTH];       /* 转置后的物理块 */
static int tab5_win_left = 0, tab5_win_top = 0, tab5_win_width = 0;

/* PPA 硬件旋转路径（详见文件后半的说明）；任一步失败则为 NULL → 退回 CPU 转置 */
static ppa_client_handle_t tab5_ppa;
static uint16_t *tab5_fb;      /* DPI 帧缓冲（PSRAM，720x1280 RGB565，行跨距无填充 = 720px） */

static inline uint16_t tab5_swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

#if defined(RG_GAMEPAD_TOUCH_MAP)
/* ---- 虚拟按键可视层（在推送前合成，按键永远在最上层）------------------------
 * 为什么必须放在这里、而不是 rg_display.c：
 *   本驱动没有整屏帧缓冲 —— lcd_send_buffer() 是"算一块推一块"，lcd_sync() 是空函数；
 *   而 GUI 是立即模式，随时可能重画任意区域。在显示层"每帧末尾叠加"会被随后的 GUI
 *   绘制覆盖 => 实机表现为按键闪烁。放在推给面板之前的最后一步合成，才是稳定的。
 * scratch 里已经是面板格式（小端 565），所以这里直接写普通 RGB565 值。 */
static inline uint16_t tab5_overlay_dim(uint16_t c)
{
    return (uint16_t)((c >> 1) & 0x7BEF);   /* RGB565 每通道减半 => 同色系暗填充 */
}

/* 没有文字标签，靠"颜色 + 位置"辨认，所以每个按键给一个可区分的颜色 */
static uint16_t tab5_overlay_color(rg_key_t key)
{
    switch (key)
    {
        case RG_KEY_UP:     return 0x07E0;  /* 绿 */
        case RG_KEY_DOWN:   return 0x07FF;  /* 青 */
        case RG_KEY_LEFT:   return 0xF800;  /* 红 */
        case RG_KEY_RIGHT:  return 0xFD20;  /* 橙 */
        case RG_KEY_A:      return 0xF81F;  /* 品红 */
        case RG_KEY_B:      return 0xFFE0;  /* 黄 */
        case RG_KEY_X:      return 0x001F;  /* 蓝 */
        case RG_KEY_Y:      return 0x781F;  /* 紫 */
        case RG_KEY_L:      return 0xC618;  /* 浅灰 */
        case RG_KEY_R:      return 0x8410;  /* 深灰 */
        case RG_KEY_SELECT: return 0xAFE0;  /* 橄榄 */
        case RG_KEY_START:  return 0xFC00;  /* 琥珀 */
        case RG_KEY_MENU:   return 0xFFFF;  /* 白 */
        default:            return 0xFFFF;
    }
}

static void tab5_overlay_blit(uint16_t *scratch, int x0, int y0, int rows, int w)
{
    size_t count = 0;
    const rg_keymap_touch_t *map = rg_input_get_touch_keymap(&count);
    if (!map || !count)
        return;

    const int border = 3;

    for (size_t i = 0; i < count; ++i)
    {
        const rg_keymap_touch_t *btn = &map[i];
        const int lx0 = btn->x - btn->w / 2;
        const int ly0 = btn->y - btn->h / 2;
        /* 逻辑 -> 物理：px = (PHYS_W-1) - ly, py = lx（与上面的转置映射一致） */
        const int bx0 = TAB5_PHYS_W - ly0 - btn->h;
        const int by0 = lx0;
        const int bw = btn->h, bh = btn->w;

        const int ix0 = RG_MAX(bx0, x0), ix1 = RG_MIN(bx0 + bw, x0 + rows);
        const int iy0 = RG_MAX(by0, y0), iy1 = RG_MIN(by0 + bh, y0 + w);
        if (ix0 >= ix1 || iy0 >= iy1)
            continue;

        const uint16_t col = tab5_overlay_color(btn->key);
        const uint16_t fill = tab5_overlay_dim(col);

        for (int py = iy0; py < iy1; ++py)
        {
            const int ry = py - by0;
            uint16_t *dst = scratch + (size_t)(py - y0) * rows;
            for (int px = ix0; px < ix1; ++px)
            {
                const int rx = px - bx0;
                const bool edge = (rx < border || ry < border ||
                                   rx >= bw - border || ry >= bh - border);
                dst[px - x0] = edge ? col : fill;
            }
        }
    }
}
#endif /* RG_GAMEPAD_TOUCH_MAP */

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
/* 虚拟按键叠加，但直接写 DPI 帧缓冲（PPA 路径专用）。
 * ⚠ 帧缓冲由 PPA 硬件直接写、不经 CPU 缓存，所以 CPU 端的小区域写必须：
 *   ① 先失效该区域的 cache 行（否则"未对齐的局部写"会把过期行内容写回去，抹掉 PPA 的输出）
 *   ② 写像素
 *   ③ 再写回（C2M），否则 DPI 的 DMA 读不到刚写的内容
 * 范围取整个按键矩形的包围盒并向外对齐到 cache 行；失效/写回都是批量操作，每键 2 次调用。 */
static void tab5_overlay_fb(int x0, int y0, int rows, int w)
{
    if (!tab5_fb)
        return;

    size_t count = 0;
    const rg_keymap_touch_t *map = rg_input_get_touch_keymap(&count);
    if (!map || !count)
        return;

    const int border = 3;

    for (size_t i = 0; i < count; ++i)
    {
        const rg_keymap_touch_t *btn = &map[i];
        const int lx0 = btn->x - btn->w / 2;
        const int ly0 = btn->y - btn->h / 2;
        /* 逻辑 -> 物理：px = (PHYS_W-1) - ly, py = lx */
        const int bx0 = TAB5_PHYS_W - ly0 - btn->h;
        const int by0 = lx0;
        const int bw = btn->h, bh = btn->w;

        const int ix0 = RG_MAX(bx0, x0), ix1 = RG_MIN(bx0 + bw, x0 + rows);
        const int iy0 = RG_MAX(by0, y0), iy1 = RG_MIN(by0 + bh, y0 + w);
        if (ix0 >= ix1 || iy0 >= iy1)
            continue;

        const uint16_t col = tab5_overlay_color(btn->key);
        const uint16_t fill = tab5_overlay_dim(col);

        /* ① cache 行对齐的失效范围（含中间未写的行，失效干净行无害） */
        size_t b0 = ((size_t)iy0 * TAB5_PHYS_W + ix0) * 2;
        size_t b1 = ((size_t)(iy1 - 1) * TAB5_PHYS_W + ix1) * 2;
        size_t a0 = (b0 / 128) * 128;
        size_t a1 = ((b1 + 127) / 128) * 128;
        esp_cache_msync((void *)((uintptr_t)tab5_fb + a0), a1 - a0,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        /* ② 写像素 */
        for (int py = iy0; py < iy1; ++py)
        {
            const int ry = py - by0;
            uint16_t *dst = tab5_fb + (size_t)py * TAB5_PHYS_W;
            for (int px = ix0; px < ix1; ++px)
            {
                const int rx = px - bx0;
                const bool edge = (rx < border || ry < border ||
                                   rx >= bw - border || ry >= bh - border);
                dst[px] = edge ? col : fill;
            }
        }

        /* ③ 写回，让 DPI 的 DMA 看到 */
        esp_cache_msync((void *)((uintptr_t)tab5_fb + a0), a1 - a0,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
}
#endif /* RG_GAMEPAD_TOUCH_MAP */

/* DPI panel 在 use_dma2d=1 时 draw_bitmap 是异步的：忙时返回 ESP_ERR_INVALID_STATE，
 * 一帧内连续推送时会正常出现，等一下就重试（不是错误，别当故障处理）。 */
static esp_err_t tab5_draw(int x0, int y0, int x1, int y1, const uint16_t *data)
{
    esp_err_t err = ESP_FAIL;
    /* DMA 忙时 draw_bitmap 返回 ESP_ERR_INVALID_STATE —— 这是正常背压，不是故障。
     * ⚠ 但别把它当"等一会儿就好"无限等：旧代码重试 500 次 × vTaskDelay(1)=10ms
     *   => 单个区块最多阻塞 5 秒；播游戏（CPU 100%、DMA 持续忙）时会连续触发 =>
     *   主循环整段卡在 vTaskDelay 里（日志特征：BUSY 掉到 0%、FPS 归零）=> 被
     *   retro-go 的"应用无响应"看门狗 RG_PANIC("Application terminated!") 杀掉。
     * 现在只等有限时间，超时就【丢掉这一块】（下一帧会重画，宁可掉一块也不能卡死），
     * 并限频告警，好让 crash.log 里能看到丢块频率。 */
    const int max_tries = 8;   /* 8 × 1 tick(10ms) = 最多 80ms */
    for (int tries = 0; tries < max_tries; ++tries) {
        err = esp_lcd_panel_draw_bitmap(tab5_panel, x0, y0, x1, y1, data);
        if (err != ESP_ERR_INVALID_STATE)
            return err;
        vTaskDelay(1);
    }
    static uint32_t dropped;
    if ((dropped++ % 60) == 0)
        RG_LOGW("draw busy: dropped block <%d,%d %d,%d> (total dropped=%u)\n", x0, y0, x1, y1, (unsigned)dropped);
    return err;
}

static void lcd_set_backlight(float percent)
{
    float level = RG_MIN(RG_MAX(percent / 100.f, 0.f), 1.f);
    if (tab5_panel) {
        bsp_display_brightness_set((int)(level * 100));
        RG_LOGI("backlight set to %d%%\n", (int)(level * 100));
    }
}

static void lcd_init(void)
{
    /* 8.31：背光必须先初始化，且先置 0 避免上电白闪 */
    bsp_display_brightness_init();
    bsp_display_brightness_set(0);

    /* ⚠⚠ 关键一步：Tab5 的 LCD_RST(P4) / TP_RST(P5) 由挂在 I2C 上的 IO 扩展器
     * PI4IOE5V6416 驱动，而这个初始化 BSP 自己不调用（必须由 app 调，M5 的例程在 app_main 里调）。
     * 不调它 → 扩展器上电默认输出寄存器全 0 → LCD_RST=0 / TP_RST=0：
     *   - 触摸 IC(0x55) 在 I2C 上不应答 → bsp_detect_display_type() 探不到屏型
     *     （日志表现：No known touch controller detected, defaulting to ILI9881C）
     *   - 面板被按在复位里 → 面板初始化不完成、屏幕全黑（背光也没用）
     * 扩展器是独立芯片，寄存器状态跨 ESP 复位保留，所以"真断电"后必须由固件重新初始化它。
     * 必须放在 _to_st7123 之前，否则探测拿不到 ST7123 会走错分支。 */
    {
        extern esp_err_t bsp_i2c_init(void);
        extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);
        extern void bsp_io_expander_pi4ioe_init(i2c_master_bus_handle_t bus_handle);
        if (bsp_i2c_init() == ESP_OK) {
            bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
            RG_LOGI("PI4IOE expander init: LCD_RST/TP_RST released\n");
            /* 复位释放后给触摸 IC 一点时间再探测：否则紧随其后的屏型探测会探不到 ST7123
             * （BSP 探测只是决定 ST7121/ST7123，探不到会退回 ILI9881C 分支）。 */
            vTaskDelay(pdMS_TO_TICKS(150));
        } else {
            RG_LOGE("bsp_i2c_init failed, expander NOT initialized (panel/touch stay in reset)\n");
        }
    }

    /* ⚠ 绝对不要用 bsp_display_get_panel_ic() 先做"屏型识别"再选初始化路径！
     * 它内部会 i2c_master_probe 触摸 IC(0x55) 来推断屏型，实测在显示/I2C 尚未就绪时
     * 探测必然失败 -> 返回 ILI9881C -> 走进 ILI9881C 路径后 abort()，
     * 真机表现是"背光一闪 + 崩溃重启循环"，日志特征：
     *   W M5STACK_TAB5: No known touch controller detected, defaulting to ILI9881C
     *   W ledc: GPIO 22 is not usable ... / abort() was called
     * （自检固件 M2a/M2c 之所以能亮，就是因为它直接调下面这个 _to_st7123，没做探测。）
     * 本机面板是 ST7123 一体屏，直接走 ST7123 初始化路径。 */
    bsp_lcd_handles_t handles = {0};
    esp_err_t err = bsp_display_new_with_handles_to_st7123(NULL, &handles);
    if (err != ESP_OK || !handles.panel) {
        RG_LOGW("st7123 path failed (err=0x%x), trying generic path\n", err);
        err = bsp_display_new_with_handles(NULL, &handles);   /* 其它屏型（本机未实测） */
    }

    if (err != ESP_OK || !handles.panel) {
        RG_LOGE("Tab5 display init FAILED (err=0x%x)\n", err);
        bsp_display_brightness_set(60);   /* 失败也点背光：区分"通路没起来"和"屏没亮" */
        return;
    }
    tab5_panel = handles.panel;

    /* 拿 DPI 帧缓冲并注册 PPA 客户端（PPA 直接写它，不经过 CPU 缓存）。
     * 任何一步失败都只是退回 CPU 转置路径，不影响功能。 */
    {
        void *fb = NULL;
        /* 运行时开关（免刷机 A/B）：默认**不启用** PPA（走已知可用的 CPU 转置路径）。
         * 想试 PPA 就在 SD 根目录放一个 ppa_on 文件，删掉即退回。
         * 原因：PPA 是硬件 master，写帧缓冲时可能与模拟器抢 PSRAM 带宽，
         * 实测出现过进游戏后卡死，需要先定位再决定是否默认开启。 */
        bool ppa_allowed = false;
        FILE *on = fopen("/sd/ppa_on", "r");
        if (on) {
            ppa_allowed = true;
            fclose(on);
        }
        if (esp_lcd_dpi_panel_get_frame_buffer(tab5_panel, 1, &fb) == ESP_OK && fb && ppa_allowed) {
            tab5_fb = (uint16_t *)fb;
            ppa_client_config_t ppa_cfg = {
                .oper_type = PPA_OPERATION_SRM,
                .max_pending_trans_num = 2,
            };
            if (ppa_register_client(&ppa_cfg, &tab5_ppa) == ESP_OK)
                RG_LOGI("PPA SRM ready (fb=%p, %dx%d, stride=%dpx)\n", fb, TAB5_PHYS_W, TAB5_PHYS_H, TAB5_PHYS_W);
            else {
                tab5_ppa = NULL;
                RG_LOGW("ppa_register_client failed -> CPU transpose path\n");
            }
        } else {
            RG_LOGW("PPA disabled (default; put /sd/ppa_on to enable) -> CPU transpose path\n");
        }
    }

    RG_SCREEN_INIT();   /* 约定钩子：显示初始化必须经这个宏（历史教训：漏掉 = 黑屏） */

    lcd_set_backlight(80);
    RG_LOGI("Tab5 DSI ready (ST7123 path): logical %dx%d -> physical %dx%d (90CW map)\n",
            RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, TAB5_PHYS_W, TAB5_PHYS_H);
}

/* 前置声明：lcd_deinit 先用到它，定义在下方（性能打点小节） */
static void tab5_perf_flush(const char *app);

static void lcd_deinit(void)
{
    if (tab5_panel) {
        /* 退出应用前把本场性能汇总落盘（/sd/perf.log），不用串口也能判读瓶颈 */
        rg_app_t *app = rg_system_get_app();
        tab5_perf_flush(app && app->name ? app->name : "?");
        bsp_display_brightness_set(0);
        esp_lcd_panel_del(tab5_panel);
        tab5_panel = NULL;
    }
}

/* DSI 没有地址窗口寄存器：只记下来，推送时用（坐标是逻辑横向空间的） */
static void lcd_set_window(int left, int top, int width, int height)
{
    if (left < 0 || top < 0 || width <= 0 || height <= 0 ||
        left + width > RG_SCREEN_WIDTH || top + height > RG_SCREEN_HEIGHT) {
        RG_LOGW("Bad lcd window (x0=%d, y0=%d, w=%d, h=%d)\n", left, top, width, height);
    }
    tab5_win_left  = left;
    tab5_win_top   = top;
    tab5_win_width = width;
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    RG_ASSERT_ARG(length <= LCD_BUFFER_LENGTH);
    return tab5_line_buffer;
}

/* ---- PPA SRM：硬件旋转，直接写 DPI 帧缓冲 ----------------------------------
 * 动机（实测数据）：显示任务每渲染一帧约 14ms，其中只有 ~3ms 是 CPU 转置，
 *  ~11ms 是把块刷进 PSRAM 的 cache 写回 —— 而 DPI 正以 88MB/s 持续读同一片 PSRAM
 *  （rev1.3 已知争用），CPU 写回被反复饿死。PPA 由硬件直接写帧缓冲：
 *  既不占 CPU 做转置，也不产生 cache 写回 => 每帧 ~14ms 降到 ~1-2ms，
 *  自动帧跳过策略才有余量把 frameskip 从 5 降到 1~2（画面 10fps -> 30~60fps）。
 * 任何一步失败都自动退回原来的 CPU 转置路径（tab5_ppa == NULL）。 */

/* ---------------------------------------------------------------------------
 * 性能打点：显示路径到底吃掉多少 CPU？
 * 判读方法：日志里的 BUSY% 是整机 CPU 占用（含模拟器核心 + 显示路径）。
 *   这里给出显示路径的绝对耗时 => 显示占比 = display_us / 1e6。
 *   剩下的大头就是模拟器核心（GBA 的 ARM7 解释器）。
 *   ★ 若显示占比很低（<20%），说明瓶颈在核心 —— 那 PPA 也救不了帧率，别白干。
 * 每秒在串口打一行，退出游戏时把整场汇总追加到 /sd/perf.log（不用串口也能读）。
 * ------------------------------------------------------------------------- */
static uint64_t tab5_pf_tr_us, tab5_pf_sub_us;                 /* 当前 1 秒窗口 */
static uint64_t tab5_pf_tot_tr_us, tab5_pf_tot_sub_us;         /* 整场累计 */
static int64_t  tab5_pf_win_start, tab5_pf_sess_start;
static uint32_t tab5_pf_blocks, tab5_pf_tot_blocks;

static void tab5_perf_report(void)
{
    int64_t now = esp_timer_get_time();
    if (tab5_pf_win_start == 0) {
        tab5_pf_win_start = tab5_pf_sess_start = now;
        return;
    }
    int64_t elapsed = now - tab5_pf_win_start;
    if (elapsed < 1000000)
        return;
    uint64_t total = tab5_pf_tr_us + tab5_pf_sub_us;
    RG_LOGI("PERF: display=%u.%02ums/%ums (transpose=%u.%02u submit=%u.%02u) blocks=%u\n",
        (unsigned)(total / 1000), (unsigned)((total % 1000) / 10), (unsigned)(elapsed / 1000),
        (unsigned)(tab5_pf_tr_us / 1000), (unsigned)((tab5_pf_tr_us % 1000) / 10),
        (unsigned)(tab5_pf_sub_us / 1000), (unsigned)((tab5_pf_sub_us % 1000) / 10),
        (unsigned)tab5_pf_blocks);
    tab5_pf_tr_us = tab5_pf_sub_us = 0;
    tab5_pf_blocks = 0;
    tab5_pf_win_start = now;
}

/* 退出游戏（回到 launcher）时落盘整场汇总 */
static void tab5_perf_flush(const char *app)
{
    int64_t total_ms = (esp_timer_get_time() - tab5_pf_sess_start) / 1000;
    if (tab5_pf_sess_start == 0 || total_ms <= 0)
        return;
    uint64_t disp_ms = (tab5_pf_tot_tr_us + tab5_pf_tot_sub_us) / 1000;
    FILE *f = fopen("/sd/perf.log", "a");
    if (!f) return;
    fprintf(f, "[%s] session=%lldms display=%llums (%.1f%%) transpose=%llums submit=%llums blocks=%u\n",
        app ? app : "?", (long long)total_ms, (unsigned long long)disp_ms,
        100.0 * (double)disp_ms / (double)total_ms,
        (unsigned long long)(tab5_pf_tot_tr_us / 1000),
        (unsigned long long)(tab5_pf_tot_sub_us / 1000), (unsigned)tab5_pf_tot_blocks);
    fclose(f);
    RG_LOGI("PERF: session summary written to /sd/perf.log (%llums display / %lldms total)\n",
        (unsigned long long)disp_ms, (long long)total_ms);
    tab5_pf_tot_tr_us = tab5_pf_tot_sub_us = 0;
    tab5_pf_tot_blocks = 0;
    tab5_pf_sess_start = 0;
}

/* 把 retro-go 送来的逻辑行（可能很窄，例如 4 行 x 1280 列）整体转成一块
 * 物理矩形后一次性推给面板：90° 旋转下"若干逻辑行"恰好等于"物理上的一竖条"。 */
static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (!tab5_panel || length == 0 || tab5_win_width <= 0)
        return;

    const int w = tab5_win_width;                       /* 逻辑列数 */
    const int rows = (int)(length / (size_t)w);         /* 本次的逻辑行数 */
    if (rows <= 0)
        return;

    /* 逻辑 ly = win_top + i, lx = win_left + j  ->
     * 物理 px = 719 - ly = (TAB5_PHYS_W - 1) - ly, py = lx
     * 于是 [(i,j) 块] 落在物理矩形 [x0, x0+rows) x [y0, y0+w)：
     *   x0 = TAB5_PHYS_W - win_top - rows,  y0 = win_left */
    const int x0 = TAB5_PHYS_W - tab5_win_top - rows;
    const int y0 = tab5_win_left;

    int64_t t_tr0 = esp_timer_get_time();
    bool done = false;
    esp_err_t err = ESP_OK;

    /* ---- 首选：PPA 硬件旋转（不做 CPU 转置、不产生 cache 写回）---- */
    if (tab5_ppa && tab5_fb && (((uintptr_t)buffer & 127u) == 0)) {
        ppa_srm_oper_config_t cfg = {
            .in = {
                .buffer = buffer,
                .pic_w = (uint32_t)w, .pic_h = (uint32_t)rows,
                .block_w = (uint32_t)w, .block_h = (uint32_t)rows,
                .block_offset_x = 0, .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = tab5_fb,
                .buffer_size = (uint32_t)(TAB5_PHYS_W * TAB5_PHYS_H * 2),
                .pic_w = (uint32_t)TAB5_PHYS_W, .pic_h = (uint32_t)TAB5_PHYS_H,
                /* 输出块尺寸由输入块 + 缩放 + 旋转推导（旋转 270° => rows x w），这里只给落点 */
                .block_offset_x = (uint32_t)x0, .block_offset_y = (uint32_t)y0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,   /* 逆时针 270° == 顺时针 90° */
            .scale_x = 1.0f, .scale_y = 1.0f,
            .mirror_x = false, .mirror_y = false,
            .rgb_swap = false,
            .byte_swap = true,      /* 源为 565 大端，面板要小端 */
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        err = ppa_do_scale_rotate_mirror(tab5_ppa, &cfg);
        done = (err == ESP_OK);
        if (!done)
            RG_LOGW("PPA SRM failed (0x%x), falling back to CPU transpose\n", err);
    }

    /* ---- 回退：CPU 转置 + 面板推送（原路径）---- */
    if (!done) {
        for (int i = 0; i < rows; ++i) {
            const uint16_t *src = buffer + (size_t)i * w;
            const int a = rows - 1 - i;
            for (int j = 0; j < w; ++j)
                tab5_scratch[(size_t)j * rows + a] = tab5_swap16(src[j]);
        }
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
        /* 虚拟按键在推给面板前的最后一刻合成（避免被 GUI 立即模式的重绘覆盖 => 不闪） */
        tab5_overlay_blit(tab5_scratch, x0, y0, rows, w);
#endif
        err = tab5_draw(x0, y0, x0 + rows, y0 + w, tab5_scratch);
        if (err != ESP_OK)
            RG_LOGE("draw failed (err=0x%x) at phys <%d,%d %d,%d>\n", err, x0, y0, x0 + rows, y0 + w);
    }
    int64_t t_tr1 = esp_timer_get_time();

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* PPA 路径：虚拟按键改为直接叠加到帧缓冲（必须在 PPA 之后，否则被覆盖） */
    if (done)
        tab5_overlay_fb(x0, y0, rows, w);
#endif
    int64_t t_tr2 = esp_timer_get_time();

    tab5_pf_tr_us += (uint64_t)(t_tr1 - t_tr0);
    tab5_pf_sub_us += (uint64_t)(t_tr2 - t_tr1);
    tab5_pf_tot_tr_us += (uint64_t)(t_tr1 - t_tr0);
    tab5_pf_tot_sub_us += (uint64_t)(t_tr2 - t_tr1);
    tab5_pf_blocks++;
    tab5_pf_tot_blocks++;
    tab5_perf_report();

    tab5_win_top += rows;   /* 连续推送时窗口向下推进 */
}

static void lcd_sync(void)
{
    /* draw_bitmap 内部已做 cache writeback + 等 DMA；无需额外同步 */
}

static void lcd_set_rotation(int rotation)
{
    /* 旋转已固定在驱动的映射里（90° CW），此处不额外处理 */
    (void)rotation;
}
