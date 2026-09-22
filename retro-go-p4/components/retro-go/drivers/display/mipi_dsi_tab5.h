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
#include "bsp/display.h"
#include "driver/i2c_master.h"   /* IO 扩展器 PI4IOE 的 API 需要 i2c_master_bus_handle_t */
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

static inline uint16_t tab5_swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/* DPI panel 在 use_dma2d=1 时 draw_bitmap 是异步的：忙时返回 ESP_ERR_INVALID_STATE，
 * 一帧内连续推送时会正常出现，等一下就重试（不是错误，别当故障处理）。 */
static esp_err_t tab5_draw(int x0, int y0, int x1, int y1, const uint16_t *data)
{
    esp_err_t err = ESP_FAIL;
    for (int tries = 0; tries < 500; ++tries) {
        err = esp_lcd_panel_draw_bitmap(tab5_panel, x0, y0, x1, y1, data);
        if (err != ESP_ERR_INVALID_STATE)
            return err;
        vTaskDelay(1);
    }
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

    RG_SCREEN_INIT();   /* 约定钩子：显示初始化必须经这个宏（历史教训：漏掉 = 黑屏） */

    lcd_set_backlight(80);
    RG_LOGI("Tab5 DSI ready (ST7123 path): logical %dx%d -> physical %dx%d (90CW map)\n",
            RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, TAB5_PHYS_W, TAB5_PHYS_H);
}

static void lcd_deinit(void)
{
    if (tab5_panel) {
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
     * 于是 [(i,j) 块] 落在物理矩形 [x0, x0+rows) x [y0, y0+w)，其中
     *   x0 = TAB5_PHYS_W - win_top - rows,  y0 = win_left
     * scratch 行主序下标 = b * rows + a，其中 b = j（物理行）、a = rows-1-i（物理列） */
    for (int i = 0; i < rows; ++i) {
        const uint16_t *src = buffer + (size_t)i * w;
        const int a = rows - 1 - i;
        for (int j = 0; j < w; ++j)
            tab5_scratch[(size_t)j * rows + a] = tab5_swap16(src[j]);
    }

    const int x0 = TAB5_PHYS_W - tab5_win_top - rows;
    const int y0 = tab5_win_left;
    esp_err_t err = tab5_draw(x0, y0, x0 + rows, y0 + w, tab5_scratch);
    if (err != ESP_OK)
        RG_LOGE("draw failed (err=0x%x) at phys <%d,%d %d,%d>\n", err, x0, y0, x0 + rows, y0 + w);

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
