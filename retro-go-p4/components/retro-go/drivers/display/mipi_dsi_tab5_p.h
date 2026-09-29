/* ============================================================================
 * M5Stack Tab5 (ESP32-P4) 显示驱动 — MIPI DSI / ST7123，走官方 Tab5 BSP
 * ----------------------------------------------------------------------------
 * 面板原生 = 720x1280 竖屏（真机实测：把 DPI 流切成 1280x720 + MADCTL MV(0x23)
 * 会黑屏 —— 背光亮但面板锁不住信号；M5 两家官方代码也都用竖屏时序 + 上层旋转）。
 * 【竖屏版 tab5p】逻辑画面 = 720x1280 竖屏，与面板原生同向，**线性 1:1、无旋转**：
 *     逻辑 (lx, ly)  ->  物理 (px, py) = (lx, ly)
 * 历史：2026-09-27 之前用的是 (719-ly, lx) 的 90° 映射；改线性后读写全部顺序化
 * （横屏版逐行转置每 1440B 只碰 64B，实测仅 28MB/s，顺序可达 89MB/s）。
 * ⚠ 本文件里若还有"90° / 90CW / 转置"字样，都是那次改造的遗留描述，以本行为准。
 * 四角锚点图案曾在真机目视确认映射方向（git tag v0.2）。
 *
 * 为什么显示驱动自己不做放大：游戏画面 240x160 → 3x 由逻辑层/框架的缩放完成，
 * 驱动只负责把 720x480 的逻辑条带搬进面板帧缓冲。
 * ⚠ PPA 硬件 SRM（旋转+缩放一次完成）曾是很看好的替代方案，但实测"按块调用"形态下
 * 比 CPU 交换慢约 25 倍（面板扫描与 PPA 抢同一片 PSRAM），现已无条件禁用；
 * 判据与结论见 lcd_init 中 ppa_allowed 处的长注释。
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
#include "esp_rom_sys.h"         /* esp_rom_delay_us：DMA2D 忙时的短让出（远细于 1ms tick） */
#include "hal/axi_icm_ll.h"      /* AXI-ICM QoS：给 CPU cache 写回 / DMA2D 拷贝提权（见 lcd_init） */
#include "soc/icm_sys_struct.h"  /* 读 QoS 默认值：LL 只有 setter 没有 getter，直接读寄存器结构体 */
#include <esp_heap_caps.h>       /* E0 带宽探针要显式申请 PSRAM 缓冲 */
#include "bsp/display.h"
#include "driver/i2c_master.h"   /* IO 扩展器 PI4IOE 的 API 需要 i2c_master_bus_handle_t */
#include "rg_touch_overlay.h"    /* 虚拟按键可视层（标签/透明度/按下反馈） */
#include "tab5_power.h"          /* BSP I2C + IO 扩展器 + 充电使能（两份驱动共用一份）*/
/* 注意：不要 #include "bsp/m5stack_tab5.h" —— 它的 umbrella 头会拉 lvgl.h
 * （retro-go 不编 LVGL）。需要什么就手写 extern，见下。 */

/* BSP 里这几个函数的声明被关在头文件的 LVGL 段内（BSP_CONFIG_NO_GRAPHIC_LIB=1
 * 把它们编译掉了），所以照 snowveil 的做法手写 extern 声明。 */
extern esp_err_t bsp_display_new_with_handles(
    const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles);
extern esp_err_t bsp_display_new_with_handles_to_st7123(
    const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles);

/* ---- AXI-ICM QoS 调优（2026-09-25 夜；完整证据链见 docs/NIGHT-2026-09-25-DISPLAY.md）----
 *
 * 起因：IDF 在 DSI 欠载中断里的官方提示就是"用 AXI-ICM 优化内存带宽"。P4 上确实有这套 QoS 硬件，
 * 我们的两条关键路径对应：
 *   AXI_ICM_MASTER_CACHE (1)   ← CPU 转置后的 cache 写回
 *   AXI_ICM_MASTER_DMA2D (10)  ← 推送用的异步拷贝（esp_async_fbcpy）
 * 面板扫描则走 DW-GDMA（AXI_ICM_MASTER_DW_GDMA_M0/M1）。
 *
 * 症状是"延迟被饿死"而非吞吐不够：面板持续 ~100MB/s 读 PSRAM（1280x720x2B @ ~54Hz），
 * 我们只要 ~12MB/s，但实测每次推送 0.6~1.0ms，且约一半绘制被 DMA2D 拒收丢弃
 * （日志 "previous draw operation is not finished" 每秒几十上百条）。
 *
 * 本函数只做**保守**一步：抬高我们自己两条主设备的读写 QoS。
 * **刻意不动面板的优先级/突发限制** —— 给视频流降权或限突发有过冲导致 underrun（画面变蓝）的风险，
 * 要等真机分档实测后再决定。
 * 默认值先打日志：LL 只有 setter 没有 getter，直接读寄存器结构体拿初值，便于判断还有多少提权空间。 */
static void tab5_axi_icm_tune(void)
{
    const uint32_t cache_aw = AXI_ICM.mst_awqos_reg0.reg_cache_awqos;
    const uint32_t cache_ar = AXI_ICM.mst_arqos_reg0.reg_cache_arqos;
    const uint32_t dma2d_aw = AXI_ICM.mst_awqos_reg0.reg_dma2d_awqos;
    const uint32_t dma2d_ar = AXI_ICM.mst_arqos_reg0.reg_dma2d_arqos;
    RG_LOGI("AXI-ICM qos defaults: cache w/r=%u/%u dma2d w/r=%u/%u\n",
            (unsigned)cache_aw, (unsigned)cache_ar, (unsigned)dma2d_aw, (unsigned)dma2d_ar);
    axi_icm_ll_set_cache_qos_arbiter_prio(15, 15);   /* CPU cache 读写提权 */
    axi_icm_ll_set_dma2d_qos_arbiter_prio(15, 15);   /* 推送拷贝读写提权 */
    RG_LOGI("AXI-ICM qos raised: cache w/r=15/15 dma2d w/r=15/15\n");
}

#define TAB5_PHYS_W 720    /* 面板物理（竖屏）宽 */
#define TAB5_PHYS_H 1280   /* 面板物理（竖屏）高 */

static esp_lcd_panel_handle_t tab5_panel = NULL;
/* ⚠ 这两个缓冲必须 128 字节对齐：PPA 硬件路径有硬性要求（见下方 lcd_send_buffer 的条件判断），
 * 不对齐时它会**静默**退回 CPU 转置 —— 2026-09-25 就是栽在这里：日志打了 "PPA SRM ready"，
 * 实际每次都在走 CPU 路径，白跑一整轮 A/B。链接后的地址是 0x...bbc（低 7 位非零），所以必须显式声明。 */
static uint16_t tab5_line_buffer[LCD_BUFFER_LENGTH] __attribute__((aligned(128)));  /* retro-go 收集逻辑行用 */
static uint16_t tab5_scratch[LCD_BUFFER_LENGTH] __attribute__((aligned(128)));      /* 转置后的物理块 */

/* E3（2026-09-25）：源数据先**整块顺序读**进片内 SRAM，再从 SRAM 转置。
 * 依据 BW2 打点：同样的"每 1440B 碰 64B"模式，打在 PSRAM 上只有 28.2MB/s，
 * 而顺序读有 89MB/s（差 3 倍）。块的源数据本身连续（每行 1440B 紧邻），
 * 所以整块读就是一次纯顺序读；坏模式随后打在 SRAM 上，没有行缓冲惩罚。
 * 块超过缓冲（48KB）时自动退回原分块路径，行为不变。 */
#define TAB5_STAGE_BYTES (48 * 1024)
static uint16_t tab5_stage[TAB5_STAGE_BYTES / 2] __attribute__((aligned(128)));
static int tab5_win_left = 0, tab5_win_top = 0, tab5_win_width = 0;

/* PPA 硬件旋转路径（详见文件后半的说明）；任一步失败则为 NULL → 退回 CPU 转置 */
static ppa_client_handle_t tab5_ppa;
static uint16_t *tab5_fb;      /* DPI 帧缓冲（PSRAM，720x1280 RGB565，行跨距无填充 = 720px） */

/* ── E0：裸测 PSRAM 带宽（2026-09-25，一次性探针，测完即删）────────────────────
 * 要回答的问题：现在只知道"穿过整条显示通路是 29.7MB/s"，**不知道内存本身能给多少**。
 * 没有这个地板值，后面所有优化（双缓冲、改块形状、去掉旋转）都是瞎猜。
 *
 * 方法：同一段代码在两个时刻各跑一次 ——
 *   A 面板开始扫描之前（bsp_display_new 之前）→ 没有面板抢带宽
 *   B 面板开始扫描之后                  → 面板正以约 107MB/s 持续读帧缓冲
 * 两个数之差 = 面板扫描到底吃掉了多少内存带宽（这是我们一直在猜的事）。
 *
 * 缓冲 8MB：**必须远大于 cache**，否则读全部命中 cache，测的是 cache 不是 PSRAM。
 * 三个动作分别计时：顺序写（含把脏数据推下 PSRAM 的 msync）/ 顺序读（先失效 cache）。
 * ⚠ 探针在扫描期间会造成几毫秒重负载，屏幕可能闪一下 —— 预期行为，不是坏了。 */
#define TAB5_BW_BYTES (8 * 1024 * 1024)
#define TAB5_BW_WORDS (TAB5_BW_BYTES / 4)

static void tab5_bw_probe(const char *tag)
{
    uint32_t *buf = heap_caps_malloc(TAB5_BW_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) { RG_LOGW("BW[%s] PSRAM alloc failed\n", tag); return; }

    int64_t t0 = esp_timer_get_time();
    for (size_t i = 0; i < TAB5_BW_WORDS; ++i)
        buf[i] = (uint32_t)i;                                          /* 顺序写 */
    esp_cache_msync(buf, TAB5_BW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M); /* 真正落到 PSRAM 才算完 */
    int64_t t1 = esp_timer_get_time();

    esp_cache_msync(buf, TAB5_BW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C); /* 失效，保证读来自 PSRAM */
    uint32_t sum = 0;
    for (size_t i = 0; i < TAB5_BW_WORDS; ++i)
        sum += buf[i];                                                 /* 顺序读 */
    int64_t t2 = esp_timer_get_time();

    RG_LOGI("BW[%s] write=%.1f MB/s  read=%.1f MB/s  (写 %.1fms / 读 %.1fms, 缓冲 8MB, sum=%u)\n",
            tag,
            (TAB5_BW_BYTES / 1e6) / ((t1 - t0) / 1e6),
            (TAB5_BW_BYTES / 1e6) / ((t2 - t1) / 1e6),
            (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (unsigned)sum);

    heap_caps_free(buf);
}

/* ── E0b：用**我们真实的访问模式**再测一遍（2026-09-25，一次性探针）─────────────
 * E0 测的是顺序访问（89MB/s），但转置的真实负载是跨行访问：
 *   读：每行连续读 32 像素 = 正好一整条 64B cache line，然后跳到下一行（跨 1440B）
 *   写：旧转置每行只写 2 字节（跨 1440B）—— 那个"28 倍写放大"到底多贵
 * 单位统一用**有效字节/秒**，这样能直接和通路的 29.7MB/s 对比，看是谁在拖后腿。
 * PASSES 次循环是为了让耗时足够长（8MB 单趟不到 1ms，测不准）。 */
#define TAB5_BW2_PASSES 20

static void tab5_bw_probe_strided(const char *tag)
{
    const int STRIDE_PX = 720;                              /* 与帧缓冲同跨距 */
    const int ROWS = TAB5_BW_BYTES / (STRIDE_PX * 2);
    uint16_t *buf = heap_caps_malloc(TAB5_BW_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) { RG_LOGW("BW2[%s] PSRAM alloc failed\n", tag); return; }

    /* ① 转置的读：每行读 32 像素（一条 64B line），跳下一行 */
    int64_t t0 = esp_timer_get_time();
    uint32_t sum = 0;
    for (int p = 0; p < TAB5_BW2_PASSES; ++p) {
        esp_cache_msync(buf, TAB5_BW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        for (int r = 0; r < ROWS; ++r)
            for (int x = 0; x < 32; ++x)
                sum += buf[(size_t)r * STRIDE_PX + x];
    }
    int64_t t1 = esp_timer_get_time();

    /* ② 旧转置的写：每行只写 2 字节（跨 1440B） */
    for (int p = 0; p < TAB5_BW2_PASSES; ++p)
        for (int r = 0; r < ROWS; ++r)
            buf[(size_t)r * STRIDE_PX] = (uint16_t)(r + p);
    esp_cache_msync(buf, TAB5_BW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    int64_t t2 = esp_timer_get_time();

    const double used_read  = (double)ROWS * 32 * 2 * TAB5_BW2_PASSES / 1e6;  /* 有效 MB */
    const double used_write = (double)ROWS * 2 * TAB5_BW2_PASSES / 1e6;

    RG_LOGI("BW2[%s] 跨行读64B=%.1f MB/s(有效) 跨行写2B=%.1f MB/s(有效) [读 %.1fms / 写 %.1fms, sum=%u]\n",
            tag, used_read / ((t1 - t0) / 1e6), used_write / ((t2 - t1) / 1e6),
            (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (unsigned)sum);

    heap_caps_free(buf);
}

static inline uint16_t tab5_swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/* ---- 虚拟按键可视层 ---------------------------------------------------------
 * 实现在 rg_touch_overlay.c（目标无关的共享模块：标签 / 透明度 / 按下反馈都在那里），
 * 本驱动只负责在"推给面板前的最后一刻"把它合成进来。
 * 为什么必须在显示层合成、不能在 GUI 层画：本驱动没有整屏帧缓冲 —— lcd_send_buffer()
 * 是"算一块推一块"，lcd_sync() 是空函数；而 GUI 是立即模式，随时可能重画任意区域。
 * 在 GUI 层画的按键会被随后的绘制覆盖 => 实机表现为按键闪烁（27b44b4 修过这个）。
 * 两条渲染路径都要合成：CPU 转置（写 scratch）和 PPA 直写帧缓冲（写 tab5_fb）。 */

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

    /* ⚠⚠ 关键一步：电源/复位外围（BSP I2C → IO 扩展器 → 充电使能）现在统一收在
     * tab5_power.h 的 tab5_power_init() 里，**横屏/竖屏两份显示驱动共用同一个实现**
     * （原先只写在竖屏版里 → 充电使能的修复漏了横屏一份，谁切横屏构建就复发，
     *  见 docs/CODE-REVIEW-v0.4.1.md P1-1）。
     * 为什么必须调、为什么顺序不能动：见 tab5_power.h 文件头的完整说明。
     * 必须放在 _to_st7123 之前，否则屏型探测拿不到 ST7123 会走错分支。 */
    tab5_power_init();

    /* ⚠ 绝对不要用 bsp_display_get_panel_ic() 先做"屏型识别"再选初始化路径！
     * 它内部会 i2c_master_probe 触摸 IC(0x55) 来推断屏型，实测在显示/I2C 尚未就绪时
     * 探测必然失败 -> 返回 ILI9881C -> 走进 ILI9881C 路径后 abort()，
     * 真机表现是"背光一闪 + 崩溃重启循环"，日志特征：
     *   W M5STACK_TAB5: No known touch controller detected, defaulting to ILI9881C
     *   W ledc: GPIO 22 is not usable ... / abort() was called
     * （自检固件 M2a/M2c 之所以能亮，就是因为它直接调下面这个 _to_st7123，没做探测。）
     * 本机面板是 ST7123 一体屏，直接走 ST7123 初始化路径。 */
    bsp_lcd_handles_t handles = {0};
    /* AXI-ICM QoS：先把 CPU cache 写回与 DMA2D 拷贝的优先级提起来（面板不动，理由见函数注释） */
    tab5_axi_icm_tune();

#if 0 /* E0/E0b 探针：已测完（结果见 docs），关掉以免开机时出现可见闪烁 */
    tab5_bw_probe("A-面板未扫描");   /* E0 探针 A：此时 DPI 还没起来，没有面板抢带宽 */
    tab5_bw_probe_strided("A-面板未扫描");   /* E0b：真实访问模式（跨行读 64B / 跨行写 2B） */
#endif
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
        /* ⛔ PPA 硬件旋转：2026-09-25 实测**比 CPU 转置慢约 25 倍**，无条件禁用。
         * 数据（同一台设备、同一段开机渲染负载，唯一变量是这条路径）：
         *   CPU 转置 : 每 block 0.6~0.9ms，每秒 display 47~300ms，DSI 拒收报错 317 条
         *   PPA SRM  : 每 block ~25ms，每秒 display 500~1040ms，画面掉到 1~2fps
         *              （DSI 拒收报错归零 —— 说明 PPA 确实在执行，不是又一次静默退回）
         * 根因判断：PPA 写 DPI 帧缓冲时与 DPI 以 88MB/s 持续扫描读取同一片 PSRAM 抢带宽
         * （即当年注释里的猜测，现在有实测数据）；且 PPA_TRANS_MODE_BLOCKING 下 CPU 还要
         * 等硬件搬完，省下的 CPU 时间被等待加倍还回去。
         * 重开前必读：① 先确认 tab5_line_buffer/tab5_scratch 仍是 128 字节对齐（曾经不对齐
         * 导致**静默**退回，白跑一整轮 A/B，见上方声明处注释）；② /sd/ppa_on 这个文件开关已
         * 废除（太容易被 macOS 建成 ppa_on.command，且无法反映真实执行路径）。 */
        /* 2026-09-25 第二次重测结论：**仍然 1 帧** —— 补上 data_burst_length=128 无效，
         * 所以 burst 不是病根（我先前的判断错了，记录在案）。
         * 真正的差别在**调用方式**：R8T5 是「一帧一次 PPA」（整个画面一次转完），
         * 我们是「一块一次 PPA」—— 每条 25.6 行的带子都单独发一次**阻塞** op，
         * 每秒几十次，每次都有固定开销 + 目的地是"720 行各写 51 字节"的跨行零碎写。
         * 结论：PPA 要用就**整帧一次**用，绝不能按块用。先关掉，别再挡路。 */
        /* ⚠⚠ 打开这里之前必须先重写下面的 PPA 分支：它仍按**老 90° 映射**写的
         * （ANGLE_270 + out.block_offset=(x0,y0) + byte_swap），与本文件现在的
         * **线性 1:1** 映射不兼容 —— 直接置 true 会得到旋转错位的画面（走查 P1-3）。
         * 另外"按块调用 PPA 比 CPU 交换慢约 25 倍"的实测结论依然成立（见下方长注释）。 */
        bool ppa_allowed = false;
        /* 无论 PPA 开不开都要缓存帧缓冲指针：每秒一次的叠加层刷新（屏幕帧率数字）要直接写它。
         * 原先只在 PPA 分支里赋值，导致 PPA 关着时 tab5_fb 是 NULL，叠加层无处可写。 */
        if (esp_lcd_dpi_panel_get_frame_buffer(tab5_panel, 1, &fb) == ESP_OK && fb)
            tab5_fb = (uint16_t *)fb;
        if (tab5_fb && ppa_allowed) {
            /* 2026-09-25 重测（依据 R8T5 — github.com/Layer812/R8T5，Tab5 上的 PICO-8 模拟器，
             * 它的 PPA 呈现函数注释就写着 "fast present"）：
             * 它的配置与本处只差两项 ——
             *   .data_burst_length = PPA_DATA_BURST_LENGTH_128  ← **我们原来完全没设**
             *   .max_pending_trans_num = 1                      ← 我们原来是 2
             * burst 长度没设 = DMA 每次只搬一点点，和我们 CPU 转置踩的是同一个坑，
             * 只是这次踩在 DMA 引擎里。上次"PPA 慢 25 倍"很可能就是这一项造成的。
             * 判据：屏幕帧率数字 + xpose=/draw= 打点；不对就一次构建回退。 */
            ppa_client_config_t ppa_cfg = {
                .oper_type = PPA_OPERATION_SRM,
                .max_pending_trans_num = 1,
                .data_burst_length = PPA_DATA_BURST_LENGTH_128,
            };
            if (ppa_register_client(&ppa_cfg, &tab5_ppa) == ESP_OK)
                RG_LOGI("PPA SRM ready (fb=%p, %dx%d, stride=%dpx)\n", fb, TAB5_PHYS_W, TAB5_PHYS_H, TAB5_PHYS_W);
            else {
                tab5_ppa = NULL;
                RG_LOGW("ppa_register_client failed -> CPU transpose path\n");
            }
        } else {
            RG_LOGW("PPA disabled (measured ~25x slower than CPU transpose) -> CPU transpose path\n");
        }
    }

#if 0 /* E0/E0b 探针：已测完（结果见 docs），关掉以免开机时出现可见闪烁 */
    tab5_bw_probe("B-面板扫描中");   /* E0 探针 B：面板此刻正持续读帧缓冲，抢带宽的对照 */
    tab5_bw_probe_strided("B-面板扫描中");   /* E0b：真实访问模式 */
#endif
    RG_SCREEN_INIT();   /* 约定钩子：显示初始化必须经这个宏（历史教训：漏掉 = 黑屏） */

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* 虚拟按键预渲染（一次性，约 100ms）：标签/边框/透明度的形状都在这一步烘成掩码，
     * 每帧只做查表混合。必须等显示起来后再建（建层本身不碰屏幕，但读键位表/存 NVS）。 */
    rg_overlay_init();
#endif

    lcd_set_backlight(80);
    RG_LOGI("Tab5 DSI ready (ST7123 path): logical %dx%d -> physical %dx%d (linear 1:1 map)\n",
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
        /* ⚠ 必须跟着清掉：帧缓冲属于面板，del 之后它已失效，而
         * tab5_batt_led_refresh() / tab5_perf_report() 只判 tab5_fb != NULL 就往里写
         * → 应用切换窗口期存在悬空写（走查 P2-5）。 */
        tab5_fb = NULL;
    }
}

/* DSI 没有地址窗口寄存器：只记下来，推送时用（坐标是逻辑横向空间的） */
static void lcd_set_window(int left, int top, int width, int height)
{
    /* ⚠ 命中窗口的有效性由调用方保证（框架只会给出屏幕内的窗口）。
     * 越界这里只告警**不夹取**：夹取会把"框架算错窗口"这种真 bug 掩盖成画歪，
     * 而告警能在 crash.log/串口里直接看到是谁给的错窗口。 */
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
/* 2026-09-25 夜：把 transpose= 这一坨拆开，定位那 0.6~1.0ms/次到底花在哪一段。
 * 三个候选：CPU 转置访存 / 虚拟键合成 / DMA 提交与 cache 同步。
 * 同时统计平均每次推送的行数（rows_avg/max），确认分块有没有被滤波或缓冲上限削碎。 */
static uint64_t tab5_pf_xp_us, tab5_pf_ov_us, tab5_pf_dr_us;
static uint64_t tab5_pf_tot_xp_us, tab5_pf_tot_ov_us, tab5_pf_tot_dr_us;
static uint32_t tab5_pf_px, tab5_pf_tot_px;                     /* 窗口内推送的像素数 */
static uint32_t tab5_pf_rows_max;

/* ── 整帧一次 PPA 的耗时测量（唯一还没验证过的形态，2026-09-25）
 * 背景：按块调用 PPA 是死路（每秒几十次阻塞 op，真机实测 1 帧）。R8T5 的 "fast present"
 * 是**整帧一次**：整个画面一次转完。这里只测一件事 —— 一次 op 把整块逻辑画面
 * （1280x720 RGB565 = 1.84MB）转进面板帧缓冲要多久。
 * 判据：< 5ms → "模拟器画进逻辑帧缓冲 + 每帧一次 PPA" 这条架构可行；
 *       > 50ms → PPA 在这块板子上彻底结案，不再碰。
 * 副作用：会把一帧内容写进面板帧缓冲 → 开机后约 1 秒屏幕闪一下黑，随后 launcher 重画。 */
static void tab5_ppa_frame_probe(void)
{
    /* 真实用例（2026-09-28）：模拟器帧 240x160 RGB565 --3x--> 720x480，
     * 即竖屏顶部游戏区那一块。纯缩放、不旋转（竖屏下逻辑与面板同向）。
     * 跑 5 次：第一次是冷启动（上次实测 44ms 是冷值 + 1.84MB 全帧，不具代表性），
     * 看的是**稳定态**每帧代价 —— 这才是决定能不能进生产路径的数字。 */
    const int SRC_W = 240, SRC_H = 160;
    const int DST_W = 720, DST_H = 480;
    const size_t src_bytes = (size_t)SRC_W * SRC_H * 2;      /* 76,800 B */
    const size_t out_bytes = (size_t)DST_W * DST_H * 2;      /* 691,200 B */
    const int RUNS = 5;

    uint16_t *src = (uint16_t *)heap_caps_aligned_alloc(128, src_bytes, MALLOC_CAP_SPIRAM);
    if (!src) { RG_LOGW("PPA-REAL: 源缓冲申请失败，跳过\n"); return; }
    memset(src, 0, src_bytes);
    uint16_t *dst = (uint16_t *)heap_caps_aligned_calloc(64, 1, out_bytes,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!dst) { RG_LOGW("PPA-REAL: 输出缓冲申请失败，跳过\n"); heap_caps_free(src); return; }

    ppa_client_handle_t cli = NULL;
    ppa_client_config_t ccfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&ccfg, &cli) != ESP_OK || !cli)
    {
        RG_LOGW("PPA-REAL: client 注册失败，跳过\n");
        heap_caps_free(src); heap_caps_free(dst);
        return;
    }

    RG_LOGI("PPA-REAL: %dx%d -> %dx%d (3x, 纯缩放) src=%p dst=%p\n",
            SRC_W, SRC_H, DST_W, DST_H, src, dst);

    for (int i = 0; i < RUNS; ++i)
    {
        ppa_srm_oper_config_t op = {0};
        op.in.buffer = src;
        op.in.pic_w = SRC_W; op.in.pic_h = SRC_H;
        op.in.block_w = SRC_W; op.in.block_h = SRC_H;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = dst;
        op.out.buffer_size = out_bytes;          /* 缺它必得 0x102 —— 已实测确认 */
        op.out.pic_w = DST_W; op.out.pic_h = DST_H;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;   /* 竖屏：不旋转 */
        op.scale_x = 3.0f;
        op.scale_y = 3.0f;
        op.out.block_offset_x = 0;
        op.out.block_offset_y = 0;

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = ppa_do_scale_rotate_mirror(cli, &op);
        int64_t dt = esp_timer_get_time() - t0;
        /* 只用整数格式化：ESP-IDF 日志不支持 %lld/%f */
        const int tenths = dt > 0 ? (int)((src_bytes + out_bytes) * 10 / dt) : 0;
        RG_LOGI("PPA-REAL: run#%d = %d us  (吞吐 %d.%d MB/s)  err=0x%x\n",
                i, (int)dt, tenths / 10, tenths % 10, (unsigned)err);
    }

    ppa_unregister_client(cli);
    heap_caps_free(src);
    heap_caps_free(dst);
}

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
/* 电量圆灯：直接写面板帧缓冲 + 局部 cache 写回。
 * 为什么不能走普通叠加层：驱动只把**游戏区**（逻辑 y<480）的条带推给面板，
 * 而灯在控制区 y≈1090 —— 那条路径永远覆盖不到（与帧率数字同一个坑，见下方注释）。
 * 条带起始行取 4 的倍数：帧缓冲一行 1440B，1440%128=32，只有每 4 行才落在 128B 边界，
 * msync 要求偏移与长度都是 128 的整数倍（否则静默失败/写回范围不对）。 */
static void tab5_batt_led_refresh(void)
{
    if (!tab5_fb)
        return;
    static uint16_t *last_fb = NULL;
    static int64_t last_write = 0;
    const int64_t now = esp_timer_get_time();
    /* 换了帧缓冲（进出游戏会重挂显示）或超过 5s 也强制重画一次：
     * 否则"静止的灯"（比如纯绿）在应用切换后可能一直不重画。 */
    const bool forced = (tab5_fb != last_fb) || (now - last_write > 5000000);
    if (!rg_batt_led_refresh_needed() && !forced)
        return;
    last_fb = tab5_fb;
    last_write = now;

    int x0, y0, x1, y1;
    rg_batt_led_get_band(&x0, &y0, &x1, &y1);
    /* 先擦成背景（这块控制区没有任何别的内容，纯黑）：不擦的话"灭"的相位擦不掉旧像素 */
    for (int y = y0; y < y1; ++y)
    {
        uint16_t *row = tab5_fb + (size_t)y * TAB5_PHYS_W;
        for (int x = x0; x < x1; ++x)
            row[x] = 0x0000;
    }
    rg_batt_led_draw(tab5_fb, TAB5_PHYS_W);

    const size_t off = ((size_t)y0 * TAB5_PHYS_W + x0) * 2;
    const size_t len = (size_t)(y1 - y0) * TAB5_PHYS_W * 2;
    esp_cache_msync((void *)((uintptr_t)tab5_fb + off), len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}
#endif

/* ── 竖屏"每帧交给 PPA"生产形态测量（2026-09-29）────────────────────────────
 * 动机（本机实测）：显示路径的三段开销里，**xpose 占 79%**（launcher 空闲态
 *   PERF: display=67.06/1023ms [xpose=53.08 ovl=7.56 draw=6.04] rows=30.9）
 *   → xpose 折算 0.48ms/44KB 块 = **~92MB/s**，正好等于 CPU 顺序读 PSRAM 的实测带宽。
 *   即瓶颈是"把源像素从 PSRAM 读出来"这件事本身，换字节序的算术几乎不花时间。
 *   一帧 720x480 = 691KB ⇒ 光读就要 ~7.5ms/帧，16.7ms 预算里占掉一半 ——
 *   这就是 frameskip 关不掉的第一嫌疑。
 *
 * 与 2026-09-25 那次"PPA 比 CPU 慢 25 倍"的区别（那次判定的是别的形态）：
 *   那次 = **按块 + BLOCKING + 写面板帧缓冲**，每块都在等硬件搬完，且与 DPI 抢带宽；
 *   这次直接测**真实生产的两种形态**，落点就是面板帧缓冲顶部的游戏区：
 *     A) 一帧一次：720x480 → fb@(0,0)，旋转 0 / 缩放 1 / 换字节序（"整帧交给 PPA"）
 *     B) 按块 15 次：720x31 → fb@(0,i*31)（今天的推送粒度，看每次调用的固定开销）
 *   两者都 BLOCKING —— 与今天的 CPU 路径同样"同步等完"，数字可直接对比。
 * 判据：A ≤ 3ms/帧 → 架构可行（CPU 侧只剩按键合成）；A ≳ 7ms → 与 CPU 同档，不值得改。
 * 副作用：测试内容会写进面板帧缓冲顶部（开机闪一下），随后 launcher 重画覆盖。
 * 默认 0（发行不带这个探针）。 */
#define TAB5_PPA_PROD_PROBE 0

#if TAB5_PPA_PROD_PROBE
static void tab5_ppa_prod_probe(void)
{
    if (!tab5_fb)
        return;
    const int SRC_W = 720, SRC_H = 480;
    const size_t src_bytes = (size_t)SRC_W * SRC_H * 2;
    uint16_t *src = heap_caps_aligned_alloc(128, src_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!src) { RG_LOGW("PPA-PROD: 源缓冲申请失败\n"); return; }
    for (size_t i = 0; i < src_bytes / 2; ++i)
        src[i] = (uint16_t)(0x1084 + (i & 0xFF));

    ppa_client_handle_t cli = NULL;
    ppa_client_config_t ccfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 4,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&ccfg, &cli) != ESP_OK || !cli)
    {
        RG_LOGW("PPA-PROD: client 注册失败\n");
        heap_caps_free(src);
        return;
    }

    /* A：一帧一次（整帧 720x480 → fb 顶部）*/
    for (int run = 0; run < 5; ++run)
    {
        ppa_srm_oper_config_t op = {0};
        op.in.buffer = src;
        op.in.pic_w = SRC_W; op.in.pic_h = SRC_H;
        op.in.block_w = SRC_W; op.in.block_h = SRC_H;
        op.in.block_offset_x = 0; op.in.block_offset_y = 0;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = tab5_fb;
        op.out.buffer_size = (uint32_t)(TAB5_PHYS_W * TAB5_PHYS_H * 2);
        op.out.pic_w = TAB5_PHYS_W; op.out.pic_h = TAB5_PHYS_H;
        op.out.block_offset_x = 0; op.out.block_offset_y = 0;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x = 1.0f; op.scale_y = 1.0f;
        op.mirror_x = false; op.mirror_y = false;
        op.rgb_swap = false;
        op.byte_swap = true;
        op.mode = PPA_TRANS_MODE_BLOCKING;
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = ppa_do_scale_rotate_mirror(cli, &op);
        int64_t dt = esp_timer_get_time() - t0;
        const int tenths = dt > 0 ? (int)((src_bytes + src_bytes) * 10 / dt) : 0;
        RG_LOGI("PPA-PROD A(整帧 %dx%d 阻塞): run#%d = %d us (%d.%d MB/s) err=0x%x\n",
                SRC_W, SRC_H, run, (int)dt, tenths / 10, tenths % 10, (unsigned)err);
    }

    /* B：按块 15 次（720x31 → fb 对应行带），模拟今天的推送粒度 */
    {
        const int blk_h = 31, nblk = 15;
        int64_t tot = 0;
        esp_err_t last = ESP_OK;
        for (int b = 0; b < nblk; ++b)
        {
            ppa_srm_oper_config_t op = {0};
            op.in.buffer = src;
            op.in.pic_w = SRC_W; op.in.pic_h = SRC_H;
            op.in.block_w = SRC_W; op.in.block_h = blk_h;
            op.in.block_offset_x = 0; op.in.block_offset_y = (uint32_t)(b * blk_h);
            op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            op.out.buffer = tab5_fb;
            op.out.buffer_size = (uint32_t)(TAB5_PHYS_W * TAB5_PHYS_H * 2);
            op.out.pic_w = TAB5_PHYS_W; op.out.pic_h = TAB5_PHYS_H;
            op.out.block_offset_x = 0; op.out.block_offset_y = (uint32_t)(b * blk_h);
            op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
            op.scale_x = 1.0f; op.scale_y = 1.0f;
            op.mirror_x = false; op.mirror_y = false;
            op.rgb_swap = false;
            op.byte_swap = true;
            op.mode = PPA_TRANS_MODE_BLOCKING;
            int64_t t0 = esp_timer_get_time();
            last = ppa_do_scale_rotate_mirror(cli, &op);
            tot += esp_timer_get_time() - t0;
        }
        RG_LOGI("PPA-PROD B(按块 %dx%d x%d): 合计 %d us (每块 %d us) err=0x%x\n",
                SRC_W, blk_h, nblk, (int)tot, (int)(tot / nblk), (unsigned)last);
    }

    ppa_unregister_client(cli);
    heap_caps_free(src);
}
#endif /* TAB5_PPA_PROD_PROBE */

static void tab5_perf_report(void)
{
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* 电量圆灯跟着每一次推送刷新判断（状态没变就是几次比较，可忽略） */
    tab5_batt_led_refresh();
#endif
    /* 整帧 PPA 探针：只跑一次（面板已在扫描，拿的是真实条件下的数字）。
     * 2026-09-25 结论：三种 scale 变体全部 err=0x102（ESP_ERR_INVALID_ARG），
     * 即使源与目的都已 128 对齐 —— 说明还有别的原因，且**那个 9.4ms 不能采信**。
     * 2026-09-28 重新启用：输出改自管缓冲 + 补 buffer_size，不再写帧缓冲（无面板冲突）。
     * 本次只判断 err 是否归零 —— 归零即证明 PPA 可用，下一步才谈性能。 */
    /* 2026-09-29（走查 P2-2）：探针任务已完成（结论 = PPA 可用，但"按块调用"这条路
     * 已被实测判死），把它关掉 —— 它每次开机会申请约 768KB PSRAM、跑 5 次 PPA op、
     * 打 6 行日志。要复测时改回 1。 */
#if TAB5_PPA_PROD_PROBE
    static bool ppa_prod_probe_done = false;
    if (!ppa_prod_probe_done && tab5_fb)
    {
        ppa_prod_probe_done = true;
        tab5_ppa_prod_probe();
        RG_LOGI("PPA-PROD: 探针完成（A=整帧一次 / B=按块 15 次，见上面两行读数）\n");
    }
#endif
    int64_t now = esp_timer_get_time();
    if (tab5_pf_win_start == 0) {
        tab5_pf_win_start = tab5_pf_sess_start = now;
        return;
    }
    int64_t elapsed = now - tab5_pf_win_start;
    if (elapsed < 1000000)
        return;
    uint64_t total = tab5_pf_tr_us + tab5_pf_sub_us;
    /* rows_avg：平均每次推送的行数（像素数 / 块数 / 当前逻辑宽）。用于判断分块是否被削碎。 */
    unsigned rows_avg10 = (tab5_pf_blocks && tab5_win_width > 0)
                        ? (unsigned)((uint64_t)tab5_pf_px * 10 / tab5_pf_blocks / (uint64_t)tab5_win_width) : 0;
    RG_LOGI("PERF: display=%u.%02ums/%ums (transpose=%u.%02u submit=%u.%02u) blocks=%u"
            " [xpose=%u.%02u ovl=%u.%02u draw=%u.%02u] rows=%u.%u max=%u\n",
        (unsigned)(total / 1000), (unsigned)((total % 1000) / 10), (unsigned)(elapsed / 1000),
        (unsigned)(tab5_pf_tr_us / 1000), (unsigned)((tab5_pf_tr_us % 1000) / 10),
        (unsigned)(tab5_pf_sub_us / 1000), (unsigned)((tab5_pf_sub_us % 1000) / 10),
        (unsigned)tab5_pf_blocks,
        (unsigned)(tab5_pf_xp_us / 1000), (unsigned)((tab5_pf_xp_us % 1000) / 10),
        (unsigned)(tab5_pf_ov_us / 1000), (unsigned)((tab5_pf_ov_us % 1000) / 10),
        (unsigned)(tab5_pf_dr_us / 1000), (unsigned)((tab5_pf_dr_us % 1000) / 10),
        rows_avg10 / 10, rows_avg10 % 10, (unsigned)tab5_pf_rows_max);
#if RG_OVERLAY_SHOW_FPS
    /* 2026-09-29（走查 P2-1）：整段只为"屏幕帧率数字"服务，而 RG_OVERLAY_SHOW_FPS=0 时
     * 那个数字根本不画 —— 于是每秒白做一次「擦 40x112 物理区域 + 整屏叠加层重画 + 161KB
     * cache 写回」，而且擦除区正好横切 R 肩键一角（随后被同一次重画覆盖，所以看不出瑕疵）。
     * 包进同一个开关：要看帧率时把 rg_touch_overlay.h 里那个宏改成 1 即可。 */
    /* 每秒把叠加层（屏幕帧率数字）直接合成进面板帧缓冲。
     * 为什么非这样不可：数字只有在"被推送的块正好覆盖它"时才会被重画，而游戏中的脏区
     * 极少覆盖到顶部正中 —— 真机表现就是"数字不刷新，必须点开 menu 才更新"（menu 走整屏推送）。
     * ⚠ 必须先擦除再画：这条路径不像正常推送那样把底下的游戏画面重画一遍，
     * 不擦的话新旧数字会叠在一起（真机已验证）。
     * 区域按 cw90 映射算：数字逻辑 y=44..76 → 物理 x=644..676（固定）；
     * 逻辑 x 随位数变化 → 物理 y 取一个够宽的带（1~3 位都盖得住）。
     * 黑底是刻意的：白字+黑投影本来就按深底设计，黑框还能让数字在亮场景里也看得清。 */
    if (tab5_fb)
    {
        const int cx0 = 640, cx1 = 680, cy0 = 584, cy1 = 696;   /* 物理坐标，比数字本身略大一圈 */
        for (int py = cy0; py < cy1; ++py)
        {
            uint16_t *row = tab5_fb + (size_t)py * TAB5_PHYS_W;
            for (int px = cx0; px < cx1; ++px)
                row[px] = 0x0000;
        }
        rg_overlay_blit_linear(tab5_fb, TAB5_PHYS_W, 0, 0, TAB5_PHYS_W, TAB5_PHYS_H, TAB5_PHYS_W);
        /* 只写回数字所在的那条物理行带（y=584..696），不做整帧 1.84MB 写回 ——
         * 那是上次"蓝屏不断闪烁"的最大嫌疑。偏移 840960、长度 161280，都是 128 的整数倍。 */
        const size_t off = (size_t)TAB5_PHYS_W * 2 * 584;
        const size_t len = (size_t)TAB5_PHYS_W * 2 * 112;
        esp_cache_msync((void *)((uintptr_t)tab5_fb + off), len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
#endif /* RG_OVERLAY_SHOW_FPS */

    tab5_pf_tr_us = tab5_pf_sub_us = 0;
    tab5_pf_xp_us = tab5_pf_ov_us = tab5_pf_dr_us = 0;
    tab5_pf_px = 0;
    tab5_pf_rows_max = 0;
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
    /* ⚠ ESP-IDF 的 esp_log **不支持 %llu/%lld**（长度修饰符会被吃掉 → 数字打成垃圾，
     * 后面还有参数的话会全部错位）。这里是毫秒级整数，先强转 int 再用 %d。
     * （上面那句 fprintf 是标准 stdio，所以那里用 %llu 是对的 —— 两者别混。） */
    RG_LOGI("PERF: session summary written to /sd/perf.log (%dms display / %dms total)\n",
        (int)disp_ms, (int)total_ms);
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

    /* ── 竖屏线性映射（2026-09-27，本 target 专用）───────────────────────────
     * 面板原生就是 720x1280 竖屏，所以逻辑画面直接按 1:1 落位，**不做任何旋转**：
     *   逻辑 ly = win_top + i, lx = win_left + j  ->  物理 px = lx, py = ly
     * 于是 [(i,j) 块] 落在物理矩形 [x0, x0+w) x [y0, y0+rows)（注意：与横屏版的
     * 转置版相比 x/y 跨度互换 —— 逻辑块多宽，物理块就多宽）。
     * 收益：读写全部顺序化（横屏版逐行转置时每 1440B 只碰 64B，实测仅 28MB/s，
     * 顺序读可达 89MB/s），这正是 transpose= 那 6~8ms/帧 的来源。 */
    const int x0 = tab5_win_left;
    const int y0 = tab5_win_top;

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
    else if (tab5_ppa && tab5_fb)
    {
        /* 条件不满足（源缓冲未 128 字节对齐）时**必须说话**：静默退回是 2026-09-25 白跑
         * 一整轮 A/B 的元凶 —— 日志里只有 "PPA SRM ready"，看起来一切正常，实际每次都在
         * 走 CPU 转置。只报一次，避免刷屏。 */
        static bool warned_misaligned = false;
        if (!warned_misaligned)
        {
            warned_misaligned = true;
            RG_LOGW("PPA skipped: source %p not 128-byte aligned (low 7 bits must be zero) -> CPU transpose\n",
                    (void *)buffer);
        }
    }

    /* ---- 回退：CPU 转置 + 面板推送（原路径）---- */
    if (!done) {
        int64_t t_x0 = esp_timer_get_time();
        /* ── 分块转置（2026-09-25，真机实测后重写）─────────────────────────────
         * 旧写法逐行转：内层 j 连续读（好），但目标下标 j*rows+a 的步长是 rows 个 uint16
         * （28 行时 = 56 字节），每次只写 2 字节 —— 一条 64B cache line 只被用上 2 字节，
         * 剩下 30 次写入要等后面 28 轮 i 循环才补上。等于每轮把全部 ~630 条 line 碰一遍
         * 却每条只写 2 字节，28 轮下来 ~17640 次行事务，而理论只需 630 次（约 28 倍放大）。
         * 真机打点印证：xpose= 141ms/秒，合每像素 24ns，是理论值的 ~20 倍。
         *
         * 改法：按 32×32 分块，保持"内层沿 j 连续读"（源在 PSRAM，读要一次吃满一条 line），
         * 目标写虽然步长仍是 rows，但一个分块只碰 32 条 line（32×64B = 2KB，常驻 L1），
         * 在 32 轮 i 循环里被写满 —— 两侧都变成"一条 cache line 装 32 个有效像素"。
         * 只是**存储顺序**不同，落点与取值完全一致：等价性已由 tools/test_transpose_tiling.c
         * 在 98 个尺寸组合上验证为逐字节相同（含 1 行/1 列/非 32 倍数边界）。 */
        /* E3：整块顺序读进片内 SRAM（理由见 tab5_stage 声明处）。
         * 源数据本身连续，所以这是一次纯顺序读；转置随后从 SRAM 读，不再受 PSRAM 行缓冲惩罚。
         * 块超过 48KB 缓冲则自动退回原分块路径，行为与之前完全一致。
         *
         * 2026-09-29：**竖屏线性化之后这一趟搬运已经没必要了** —— 它当年是为「转置访问」修的
         * （那时目标下标步长 = rows，一条 cache line 只被用上 2 字节，PSRAM 行缓冲被反复惩罚），
         * 而现在源块顺序 == 目标顺序，下面的字节序循环就是纯顺序读，直接读 PSRAM 没有行缓冲惩罚，
         * 反而省掉「PSRAM→SRAM 一趟 + SRAM 再读一趟」的额外访问。
         * 保留开关做 A/B：1 = 仍旧先搬进 SRAM（旧行为），0 = 直接读源（预期更快）。 */
#define TAB5_STAGE_COPY 0
#if TAB5_STAGE_COPY
        const size_t tab5_blk_bytes = (size_t)rows * w * 2;
        const uint16_t *xsrc = buffer;
        if (tab5_blk_bytes <= TAB5_STAGE_BYTES) {
            memcpy(tab5_stage, buffer, tab5_blk_bytes);
            xsrc = tab5_stage;
        }
#else
        const uint16_t *xsrc = buffer;
#endif
        /* 线性版：源块顺序 = 目标顺序，只需字节序转换（源 565 大端 → 面板小端）。
         * 纯顺序读写，无转置、无分块、无步长跳跃。 */
        {
            const size_t n = (size_t)rows * (size_t)w;
            uint16_t *d = tab5_scratch;
            /* 2026-09-28 修 bug：这里原本写的是 buffer（PSRAM 源），
             * 于是上面那次 memcpy 到片内 SRAM 的 E3 优化**从未生效** ——
             * 搬了 23KB 却被无视，循环仍逐像素读 PSRAM，实测每像素 24ns（理论值 ~20 倍）。
             * 必须读 xsrc：块 <= 48KB 时它就是片内 SRAM 里的那份。 */
            const uint16_t *sc = xsrc;
            /* 2026-09-28：逐像素 tab5_swap16 实测 14ns/像素（compute-bound，
             * 217ms/秒）。改成 32 位取样一次处理两个像素：4 条运算换 2 像素，
             * 且写出是 32 位单存。只动这里，不碰像素格式约定。 */
            {
                const size_t np = n >> 1;      /* 像素对数（块宽 720，恒为偶数）*/
                uint32_t *d32 = (uint32_t *)d;
                const uint32_t *s32 = (const uint32_t *)sc;
                for (size_t k = 0; k < np; ++k)
                {
                    uint32_t v = s32[k];
                    d32[k] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
                }
                if (n & 1)
                    d[n - 1] = tab5_swap16(sc[n - 1]);
            }
        }
        int64_t t_x1 = esp_timer_get_time();
        int64_t t_o1;
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
        /* 虚拟按键在推给面板前的最后一刻合成（避免被 GUI 立即模式的重绘覆盖 => 不闪） */
        rg_overlay_blit_linear(tab5_scratch, w, x0, y0, w, rows, w);
        t_o1 = esp_timer_get_time();
#else
        t_o1 = t_x1;
#endif
        err = tab5_draw(x0, y0, x0 + w, y0 + rows, tab5_scratch);
        /* DMA2D 忙时 IDF 会直接**丢弃**这次绘制（0 超时抢信号量 → ESP_ERR_INVALID_STATE），
         * 我们刚做完的 CPU 转置就白费了，这一帧也只能等下一帧脏区重推 ——
         * 实测每秒 317 次丢弃（≈ 推送次数的 51%），是纯浪费。
         * 有界重试：最多 5 次、每次让出 200µs（比 1ms 的 RTOS tick 细得多；
         * 用 ROM 忙等而非 vTaskDelay，既不进调度器也不会去抢总线）。
         * **必须有上限** —— 老笔记记的"队列深度 2 会楔死显示通路"就是缺限流的教训。 */
        /* 2026-09-28：5 次 → 2 次。实测丢弃是持续性的（面板 DMA 一直忙），
 * 5 次重试基本全败却白烧 ~1ms/次。丢掉的帧由脏区在下一帧补推。 */
        for (int tries = 0; err == ESP_ERR_INVALID_STATE && tries < 2; ++tries) {
            esp_rom_delay_us(200);
            err = tab5_draw(x0, y0, x0 + w, y0 + rows, tab5_scratch);
        }
        int64_t t_d1 = esp_timer_get_time();
        if (err != ESP_OK)
            RG_LOGE("draw failed (err=0x%x) at phys <%d,%d %d,%d>\n", err, x0, y0, x0 + w, y0 + rows);
        /* 三段分开记账：定位那 0.6~1.0ms/次落在哪一段 */
        tab5_pf_xp_us += (uint64_t)(t_x1 - t_x0);
        tab5_pf_ov_us += (uint64_t)(t_o1 - t_x1);
        tab5_pf_dr_us += (uint64_t)(t_d1 - t_o1);
    }
    /* 本次推送的像素数与最大行数（不分路径都统计，用于判断分块是否被削碎） */
    tab5_pf_px += (uint32_t)(rows * w);
    if ((uint32_t)rows > tab5_pf_rows_max)
        tab5_pf_rows_max = (uint32_t)rows;
    int64_t t_tr1 = esp_timer_get_time();

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* PPA 路径：虚拟按键直接叠加到帧缓冲（必须在 PPA 之后，否则被 PPA 的输出覆盖）。
     * ⚠ 帧缓冲由 PPA 硬件直接写、不经 CPU 缓存，所以 CPU 的小区域写必须：
     *   ① 先失效该区域的 cache 行（否则"未对齐的局部写"会把过期行内容写回去，抹掉 PPA 的输出）
     *   ② 写像素  ③ 再写回（C2M），否则 DPI 的 DMA 读不到刚写的内容
     * 范围就是本块自身（合成只写块内的像素），对齐到 cache 行；每块 2 次调用。 */
    if (done)
    {
        const size_t b0 = ((size_t)y0 * TAB5_PHYS_W + x0) * 2;
        const size_t b1 = ((size_t)(y0 + rows - 1) * TAB5_PHYS_W + (x0 + w - 1)) * 2 + 1;
        const size_t a0 = (b0 / 128) * 128;
        const size_t a1 = ((b1 + 127) / 128) * 128;
        esp_cache_msync((void *)((uintptr_t)tab5_fb + a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        /* ⚠ 这里传的是**整块帧缓冲**（不是本块的小缓冲），所以缓冲区原点必须是 (0,0)、
         * 裁剪区是整屏 —— 传 (x0,y0) 会让索引变成 (py-y0)*720+(px-x0)，画面整体写偏到左上角。
         * 按键自身的位置决定实际写哪儿，不需要外部裁剪。 */
        rg_overlay_blit_linear(tab5_fb, TAB5_PHYS_W, 0, 0, TAB5_PHYS_W, TAB5_PHYS_H, TAB5_PHYS_W);
        esp_cache_msync((void *)((uintptr_t)tab5_fb + a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
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
    /* 本 target 的映射是固定的：逻辑 = 物理（线性 1:1，不旋转、不镜像）。
     * 旋转由框架在逻辑侧处理，驱动不再额外消耗 —— 真正"固定"的是面板原生方向
     * （720x1280 竖屏）。要横屏得换另一份驱动（mipi_dsi_tab5.h）。 */
    (void)rotation;
}
