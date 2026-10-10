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
#if RG_TAB5_PPA_MODE == 1
#include "freertos/semphr.h"     /* PPA 非阻塞完成信号（on_trans_done → xSemaphoreGiveFromISR） */
#endif
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"    /* esp_lcd_dpi_panel_get_frame_buffer：PPA 要直接写帧缓冲 */
#include "driver/ppa.h"          /* PPA SRM 硬件旋转：替代 CPU 转置，省掉 cache 写回 */
/* ⚠ 临时实验开关（2026-10-10）：跳过叠加层合成以单独测 PPA 本体 —— **默认 0**（已实验完）。
 * 保留此开关与注释，供以后复核"25 倍到底来自哪一步"时一键复现。 */
#ifndef RG_PPA_SKIP_OVERLAY_EXP
#define RG_PPA_SKIP_OVERLAY_EXP 0
#endif
/* 实验开关（2026-10-10，**已跑完并归零**）：mode 3 尾部**只**补做帧缓冲的两次 cache 维护。
 * 实测结论（dist/p0z-F4-mode3-keepsync，240s，与 A3/C3/E3 同口径）：
 *   **补回 msync 完全无效** —— 心跳 6 行 / 提交 150，与 E3（不补）**逐项相同**。
 *   ⇒ **msync 不是承重的**，mode 3 的崩塌与那两次 cache 维护无关。
 *   同一跑还给出一个干净反证：显示路径本身毫无问题（`stage` 504µs、`ovl_sync` **仅 15µs**、
 *   PPA 每条 391–509µs），可系统仍只跑到 **0.63 提交/秒**（对照 A3 31/s、C3 2.4/s）
 *   ⇒ **慢的地方根本不在显示路径里**（窗口 10s 里显示只占 2%）。
 * 保留此开关供以后复核。默认 0 = 不参与任何现有路径。 */
#ifndef RG_PPA_KEEP_FB_SYNC_EXP
#define RG_PPA_KEEP_FB_SYNC_EXP 0
#endif
#include "esp_cache.h"           /* esp_cache_msync：按键叠加层写帧缓冲后的失效/写回 */
#include "esp_timer.h"           /* 性能打点：区分"显示路径"与"模拟器核心"的 CPU 占用 */
#include "esp_rom_sys.h"         /* esp_rom_delay_us：DMA2D 忙时的短让出（远细于 1ms tick） */
#include "hal/axi_icm_ll.h"      /* AXI-ICM QoS：给 CPU cache 写回 / DMA2D 拷贝提权（见 lcd_init） */
#include "soc/icm_sys_struct.h"  /* 读 QoS 默认值：LL 只有 setter 没有 getter，直接读寄存器结构体 */
#include <esp_heap_caps.h>       /* E0 带宽探针要显式申请 PSRAM 缓冲 */
#include "bsp/display.h"
#include "driver/i2c_master.h"   /* IO 扩展器 PI4IOE 的 API 需要 i2c_master_bus_handle_t */
#include "rg_touch_overlay.h"    /* 虚拟按键可视层（标签/透明度/按下反馈） */
#include "tab5_power.h"          /* BSP I2C + IO 扩展器 + 充电使能（两份驱动共用一份）
                                  * ⚠ 2026-10-08 修：本文件在竖屏线上漏了这一行 —— 竖屏线把
                                  * 外围电源初始化抽成 tab5_power.h 时只改了 _p.h，
                                  * 横屏这份留着 tab5_power_init() 的调用却没有 include，
                                  * 谁切横屏构建就编不过（真机复现的后果是"充电使能漏修"）。 */
/* 注意：不要 #include "bsp/m5stack_tab5.h" —— 它的 umbrella 头会拉 lvgl.h
 * （retro-go 不编 LVGL）。需要什么就手写 extern，见下。 */

/* BSP 里这几个函数的声明被关在头文件的 LVGL 段内（BSP_CONFIG_NO_GRAPHIC_LIB=1
 * 把它们编译掉了），所以照 snowveil 的做法手写 extern 声明。 */
extern esp_err_t bsp_display_new_with_handles(
    const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles);
extern esp_err_t bsp_display_new_with_handles_to_st7123(
    const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles);

/* ---- AXI-ICM QoS 调优（2026-09-25 夜；完整证据链见 docs/archive/NIGHT-2026-09-25-DISPLAY.md）----
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
/* 0.4.9：长度必须编译期 ⇒ 按**两方向最大值**分配（LCD_BUFFER_LENGTH_MAX，见 targets/tab5/geom.h）。
 * 实际使用长度仍按当前方向取（LCD_BUFFER_LENGTH / rg_geom()->lcd_rows）。 */
static uint16_t tab5_line_buffer[LCD_BUFFER_LENGTH_MAX] __attribute__((aligned(128)));  /* retro-go 收集逻辑行用 */
static uint16_t tab5_scratch[LCD_BUFFER_LENGTH_MAX] __attribute__((aligned(128)));  /* 转置后的物理块 */

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

/* ── PPA 传输模式实验（2026-10-10；docs/SPEC-PPA-NONBLOCK-EXPERIMENT.md）────────
 * 只测一个自变量：`.mode` = BLOCKING 还是 NON_BLOCKING。模式 1/2 走**同一段**提交代码，
 * 唯一差别就是这一个字段 ⇒ 两者之差 = 纯"CPU 白等硬件"那部分。
 * 模式 1 需要流水线（1 深）：提交后立刻返回，**下一次要碰源缓冲之前**才收块；
 * 源缓冲什么时候被覆写？看 rg_display.c —— 每个块开头 `lcd_get_buffer()` 之后才重填，
 * 块内多次 send 读的是同一缓冲的不同区段（互不冲突）⇒ 收块点只有三处：
 * `lcd_get_buffer()` / 本文件每次提交之前 / `lcd_sync()`。 */
#if RG_TAB5_PPA_MODE
static uint64_t tab5_pf_ppa_sub_us, tab5_pf_ppa_wait_us;      /* 当前 1 秒窗口累计 */
static uint32_t tab5_pf_ppa_ops, tab5_pf_ppa_sub_max, tab5_pf_ppa_wait_max;
#endif
/* 2026-10-10 P0：叠加层开销的**分段**计数（旧路径）与源缓冲改道耗时（mode 3）。
 * 为什么必须分段：整条路径只有一个数时，归因只能靠猜 —— 上一轮就是这样把「25 倍」
 * 猜成"抢 PSRAM 带宽"、猜了六个月。分段后一次运行就能定案。
 * 声明放在 RG_TAB5_PPA_MODE 之外：旧合成函数在任意档位都会被编译（它挂在 RG_OVERLAY_ENABLED 下）。 */
static uint64_t tab5_pf_ovl_blit_us, tab5_pf_ovl_sync_us, tab5_pf_stage_us, tab5_pf_stage_cp_us;
static uint64_t tab5_pf_gap_us;          /* 相邻两次 send 的间隔累计 = 调用方耗时（在计时窗口之外） */
static int64_t  tab5_pf_last_send_at;    /* 上次 send 起点；0 = 本窗口还没量过 */
#if RG_TAB5_PPA_MODE == 1
static SemaphoreHandle_t tab5_ppa_done;                        /* ISR 给出，收块方 take */
static volatile bool tab5_ppa_inflight;
static struct { int x0, y0, rows, w; } tab5_ppa_pend;          /* 在飞那一块的落点（叠加层推迟合成用） */
static int64_t tab5_ppa_sub_at;
static void tab5_ppa_drain(void);                              /* 前置声明：lcd_deinit / lcd_get_buffer 先用到 */

/* ISR 上下文（DMA2D 完成中断）⇒ 只能用 FromISR 版本；返回值 = 是否需要任务切换。 */
static bool tab5_ppa_done_isr(ppa_client_handle_t cli, ppa_event_data_t *ev, void *ud)
{
    (void)cli; (void)ev; (void)ud;
    BaseType_t hp = pdFALSE;
    if (tab5_ppa_done)
        xSemaphoreGiveFromISR(tab5_ppa_done, &hp);
    return hp == pdTRUE;
}
#endif

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
     *  见 docs/archive/CODE-REVIEW-v0.4.1.md P1-1）。
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
        /* ⚠ 2026-10-10 更正：上面那条「按块 PPA 慢 25 倍」的**归因是错的**，别照着它做设计。
         * 实测（docs/PPA-NONBLOCK-RESULT-2026-10-10.md）：
         *   · PPA 调用本体         : 0.59ms/块（BLOCKING） / 0.27ms/块（非阻塞）—— 都很小
         *   · 每块的**叠加层合成** : 30.4ms/块  ← 25 倍的真身
         *   · 跳过叠加层后 PPA 本体: **0.41ms/块，比 CPU 转置（1.06ms）快 2.6 倍**
         * 叠加层为什么贵：它按**整屏矩形**遍历 92 万像素去找虚拟按键（第 501 行），并做两次
         * 跨度约 **1.03MB** 的 `esp_cache_msync`（一个 27×720 列带在帧缓冲里的线性跨度）。
         * 与 PPA 旋转、传输模式（blocking/non-blocking）、PSRAM 带宽争用都无关 —— 换传输模式
         * 只能把 30.4ms 降到 1.4ms，降幅全来自"叠加层跟着挪了位置"。
         * ⇒ 「只能用整帧一次」的前提不成立；要走整帧路线，必须用修好叠加层之后的数重新推导。 */
#if RG_TAB5_PPA_MODE
        /* 2026-10-10 实验：开关打开时允许走 PPA（0 = 默认，等价于上面那段结论：关）。
         * 1/2 的差别只有 `.mode` 一个字段，见文件上方实验说明与 docs/SPEC-PPA-NONBLOCK-EXPERIMENT.md。 */
        bool ppa_allowed = true;
#else
        bool ppa_allowed = false;
#endif
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
            const bool reg_ok = (ppa_register_client(&ppa_cfg, &tab5_ppa) == ESP_OK);
            if (reg_ok)
            {
#if RG_TAB5_PPA_MODE == 1
                /* 非阻塞形态必须拿到"完成"这个事件才能安全复用源缓冲 ⇒ 注册回调 + 建信号量。
                 * 任一步失败就不能用非阻塞，**直接退回 CPU 转置并说话**（静默退回是白跑一整轮的元凶）。 */
                ppa_event_callbacks_t cbs = { .on_trans_done = tab5_ppa_done_isr };
                tab5_ppa_done = xSemaphoreCreateBinary();
                if (!tab5_ppa_done || ppa_client_register_event_callbacks(tab5_ppa, &cbs) != ESP_OK)
                {
                    RG_LOGW("PPA nonblocking: on_trans_done 注册失败 (sem=%p) -> CPU transpose path\n",
                            (void *)tab5_ppa_done);
                    ppa_unregister_client(tab5_ppa);
                    tab5_ppa = NULL;
                }
#endif
            }
            else
            {
                tab5_ppa = NULL;
                RG_LOGW("ppa_register_client failed -> CPU transpose path\n");
            }
            if (tab5_ppa)
                RG_LOGI("PPA SRM ready (fb=%p, %dx%d, stride=%dpx, mode=%d)\n",
                        fb, TAB5_PHYS_W, TAB5_PHYS_H, TAB5_PHYS_W, RG_TAB5_PPA_MODE);
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
    RG_LOGI("Tab5 DSI ready (ST7123 path): logical %dx%d -> physical %dx%d (90CW map)\n",
            RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, TAB5_PHYS_W, TAB5_PHYS_H);
}

/* 前置声明：lcd_deinit 先用到它，定义在下方（性能打点小节） */
static void tab5_perf_flush(const char *app);

static void lcd_deinit(void)
{
    if (tab5_panel) {
#if RG_TAB5_PPA_MODE == 1
        /* 收干净在飞的 PPA op 再拆面板：否则硬件可能仍在往**即将被释放的帧缓冲**里写
         * （非阻塞形态特有的收尾，blocking 形态不需要）。 */
        tab5_ppa_drain();
#endif
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
#if RG_TAB5_PPA_MODE == 1
    /* ⚠ 收块点之一，而且是**最重要**的那个：调用方拿到这块缓冲后马上就会重填它
     * （rg_display.c 每个块开头调一次本函数，块内多次 send 读的是同一缓冲的不同区段）。
     * 非阻塞形态下 PPA 可能还在读这块缓冲 ⇒ 必须在这里等它读完。放别处都不对。 */
    tab5_ppa_drain();
#endif
    return tab5_line_buffer;
}

/* ---- PPA SRM：硬件旋转，直接写 DPI 帧缓冲 ----------------------------------
 * 动机（实测数据）：显示任务每渲染一帧约 14ms，其中只有 ~3ms 是 CPU 转置，
 *  ~11ms 是把块刷进 PSRAM 的 cache 写回 —— 而 DPI 正以 88MB/s 持续读同一片 PSRAM
 *  （rev1.3 已知争用），CPU 写回被反复饿死。PPA 由硬件直接写帧缓冲：
 *  既不占 CPU 做转置，也不产生 cache 写回 => 每帧 ~14ms 降到 ~1-2ms，
 *  自动帧跳过策略才有余量把 frameskip 从 5 降到 1~2（画面 10fps -> 30~60fps）。
 * 任何一步失败都自动退回原来的 CPU 转置路径（tab5_ppa == NULL）。 */

/* ---- 叠加层合成（PPA 路径专用）+ 非阻塞收块 ----------------------------------
 * 2026-10-10：从 lcd_send_buffer 里**原样抽出**（注释全部保留、逻辑一字未改），
 * 因为非阻塞形态下它必须被推迟执行 —— 见下面的 tab5_ppa_drain()。
 * PPA 路径：虚拟按键直接叠加到帧缓冲（必须在 PPA 之后，否则被 PPA 的输出覆盖）。
 * ⚠ 帧缓冲由 PPA 硬件直接写、不经 CPU 缓存，所以 CPU 的小区域写必须：
 *   ① 先失效该区域的 cache 行（否则"未对齐的局部写"会把过期行内容写回去，抹掉 PPA 的输出）
 *   ② 写像素  ③ 再写回（C2M），否则 DPI 的 DMA 读不到刚写的内容
 * 范围就是本块自身（合成只写块内的像素），对齐到 cache 行；每块 2 次调用。 */
static void tab5_ppa_compose_overlay(int x0, int y0, int rows, int w)
{
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
#if RG_PPA_SKIP_OVERLAY_EXP
    /* ⚠ 临时实验开关（2026-10-10，根因定位用，默认 0）：整段跳过叠加层合成（全屏 blit + 2×msync）。
     * 目的只有一个：把"PPA 本体"与"合成开销"分开 —— C 组实测每块 30.4ms，其中 PPA 调用只有 0.593ms，
     * 差额全落在本函数里。跳过它就能直接读出 PPA 本体的每块代价。交付前必须确认此宏为 0。 */
    (void)x0; (void)y0; (void)rows; (void)w;
    return;
#else
    /* 本次写入的字节范围：像素 (py,px) 在帧缓冲里的偏移 = (py*TAB5_PHYS_W + px) * 2。
     * ⚠ **必须夹到帧缓冲内**：`x0+rows` / `y0+w` 由推送分块给出，续块的 rows 是定值，
     *   不保证 x0+rows <= TAB5_PHYS_W ⇒ b1/a1 会落到帧缓冲之外 ⇒ IDF 的 cache 区间查询
     *   (`esp_cache_msync` → cache_hal_vaddr_to_cache_level_id) 直接失败：
     *   `E cache: esp_cache_msync(103): invalid addr or null pointer`
     *   —— 后果不只是刷屏：这一块的**叠加层像素既没失效也没写回**（按键可能不更新）。
     *   2026-10-08 真机日志：游戏进行中成串出现 7 条（与 `previous draw operation is not
     *   finished` 交替）。a0/a1 必须保持 128 对齐（esp_cache_msync 的硬要求），所以夹的是
     *   "128 对齐的最后一个整块"，而不是直接截断长度。 */
    const size_t fb_bytes = (size_t)TAB5_PHYS_W * TAB5_PHYS_H * 2;
    const size_t b0 = ((size_t)y0 * TAB5_PHYS_W + x0) * 2;
    const size_t b1 = ((size_t)(y0 + w - 1) * TAB5_PHYS_W + (x0 + rows - 1)) * 2 + 1;
    size_t a0 = (b0 / 128) * 128;
    size_t a1 = ((b1 + 127) / 128) * 128;
    if (a1 > fb_bytes)
    {
        /* 保留一次可见证据：下次真机日志里若还出现，就能立刻区分"夹住了"与"没夹住" */
        static bool clamp_warned = false;
        if (!clamp_warned)
        {
            clamp_warned = true;
            RG_LOGW("overlay msync range clamped: block <%d,%d %dx%d> a1=%u > fb=%u\n",
                    x0, y0, rows, w, (unsigned)a1, (unsigned)fb_bytes);
        }
        a1 = fb_bytes & ~(size_t)127;
        if (a0 > a1)
            a0 = a1;            /* 整块都在 fb 外：size=0，下面跳过 msync（合成仍要做） */
    }
    const bool sync_ok = (a0 < a1);
    /* 分段计时（2026-10-10 P0）：把"cache 维护"与"按键混合循环"分开记 —— 这两段的优化手段完全不同。 */
    const int64_t t_a = esp_timer_get_time();
    if (sync_ok)
        esp_cache_msync((void *)((uintptr_t)tab5_fb + a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const int64_t t_b = esp_timer_get_time();
    /* ⚠ 这里传的是**整块帧缓冲**（不是本块的小缓冲），所以缓冲区原点必须是 (0,0)、裁剪区是整屏 ——
     * 传 (x0,y0) 会让索引变成 (py-y0)*720+(px-x0)，画面整体写偏到左上角。
     * 按键自身的位置决定实际写哪儿，不需要外部裁剪。 */
    /* ⚠⚠ 2026-10-10 更正：上面那句"不需要外部裁剪"正是**每块 30.4ms 的真因** ——
     * blit_one 的循环体是「按键矩形 ∩ 裁剪矩形」，传整屏 ⇒ **每块都把全部 13 个按键
     * （约 13.6 万像素）混合一遍**（在 PSRAM 帧缓冲上读改写，≈0.2µs/px）。
     * 而 CPU 路径传的是本块矩形（见下方 rg_overlay_blit_cw90(tab5_scratch, rows, x0, y0, ...)），
     * 只混合落在本块内的按键 ⇒ 实测 ovl 仅 3ms/s。两边差的不是朝向，是**裁剪矩形**。
     * 想在这个目标（帧缓冲、原点=整屏）上裁剪是做不到的：blit 把 (rx,ry) 当缓冲原点。
     * 正解 = mode 3：合成到本块的**暂存源缓冲**（原点=本块），随旋转一起被 PPA 转过去。 */
    rg_overlay_blit_cw90(tab5_fb, TAB5_PHYS_W, 0, 0, TAB5_PHYS_W, TAB5_PHYS_H, TAB5_PHYS_W);
    const int64_t t_c = esp_timer_get_time();
    if (sync_ok)
        esp_cache_msync((void *)((uintptr_t)tab5_fb + a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    const int64_t t_d = esp_timer_get_time();
    tab5_pf_ovl_sync_us += (uint64_t)((t_b - t_a) + (t_d - t_c));
    tab5_pf_ovl_blit_us += (uint64_t)(t_c - t_b);
#endif  /* RG_PPA_SKIP_OVERLAY_EXP */
#else
    (void)x0; (void)y0; (void)rows; (void)w;
#endif
}

#if RG_PPA_KEEP_FB_SYNC_EXP
/* 实验用（只在本开关为 1 时编入）：像素混合之外的**全部**动作 —— 与 tab5_ppa_compose_overlay()
 * 里逐字相同的区间夹取 + 两次 msync（M2C 失效 → C2M 写回），只把中间那步 blit 换成"什么都不做"。
 * 计时记进 tab5_pf_ovl_sync_us（与合成路径的"cache 维护"同一栏，便于直接对比）。
 * 用途：mode 3 尾部补做这一步 ⇒ 与 E3（不做）只差这一个变量。 */
static void tab5_ppa_fb_sync_only(int x0, int y0, int rows, int w)
{
    const size_t fb_bytes = (size_t)TAB5_PHYS_W * TAB5_PHYS_H * 2;
    const size_t b0 = ((size_t)y0 * TAB5_PHYS_W + x0) * 2;
    const size_t b1 = ((size_t)(y0 + w - 1) * TAB5_PHYS_W + (x0 + rows - 1)) * 2 + 1;
    size_t a0 = (b0 / 128) * 128;
    size_t a1 = ((b1 + 127) / 128) * 128;
    if (a1 > fb_bytes)
        a1 = fb_bytes & ~(size_t)127;
    if (a0 >= a1)
        return;
    const int64_t t_a = esp_timer_get_time();
    esp_cache_msync((void *)((uintptr_t)tab5_fb + a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const int64_t t_c = esp_timer_get_time();
    esp_cache_msync((void *)((uintptr_t)tab5_fb + a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    const int64_t t_d = esp_timer_get_time();
    tab5_pf_ovl_sync_us += (uint64_t)((t_c - t_a) + (t_d - t_c));
}
#endif

#if RG_TAB5_PPA_MODE == 1
/* 收块：等上一块 PPA 完成（完成中断里给信号），然后补做它的叠加层合成。
 * ⚠ 超时门限是 **2 秒**（2026-10-10 实测）：引擎在面板满载时偶发长尾 —— 轻载 0.6~1.2ms/op 稳定，
 *   但每次进入重载（demo 全屏变化）都会出现 11ms / 24ms / 107ms 的尖峰，并有 >1s 的整体停顿
 *   （此时 DMA2D/PSRAM 被面板扫描与推屏一起占满）。门限设 200ms 或 1s 都会在重载头几秒把 PPA 关掉，
 *   那是"守卫误伤"、拿不到重载数据；2s 是"给得起"的上限（再长就是把主循环拖进看门狗）。 */
#define TAB5_PPA_DRAIN_TIMEOUT_MS 2000
static void tab5_ppa_drain(void)
{
    if (!tab5_ppa_inflight)
        return;
    if (!tab5_ppa_done)
    {
        tab5_ppa_inflight = false;
        tab5_ppa = NULL;         /* 没有信号量就不能用非阻塞形态 */
        return;
    }
    if (xSemaphoreTake(tab5_ppa_done, pdMS_TO_TICKS(TAB5_PPA_DRAIN_TIMEOUT_MS)) == pdTRUE)
    {
        const int64_t dt = esp_timer_get_time() - tab5_ppa_sub_at;
        tab5_pf_ppa_wait_us += (uint64_t)dt;
        if (dt > (int64_t)tab5_pf_ppa_wait_max)
            tab5_pf_ppa_wait_max = (uint32_t)dt;
        tab5_ppa_inflight = false;
        /* 叠加层推迟到这里合成：块内像素现在肯定已经写完（等到了完成中断） */
        tab5_ppa_compose_overlay(tab5_ppa_pend.x0, tab5_ppa_pend.y0, tab5_ppa_pend.rows, tab5_ppa_pend.w);
        return;
    }
    static int to;
    if (to++ < 8)
        RG_LOGW("PPA done timeout (>%dms) -> PPA disabled, CPU transpose from now on\n",
                TAB5_PPA_DRAIN_TIMEOUT_MS);
    tab5_ppa_inflight = false;
    if (tab5_ppa)
    {
        ppa_unregister_client(tab5_ppa);
        tab5_ppa = NULL;
    }
}
#endif  /* RG_TAB5_PPA_MODE == 1 */

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
    const size_t src_bytes = (size_t)1280 * 720 * 2;
    /* PPA 要求缓冲 128 字节对齐（不对齐会被拒 —— 我们生产代码里就吃过这个亏）。
     * heap_caps_malloc 不保证 128 对齐，必须显式 aligned_alloc。 */
    uint16_t *src = (uint16_t *)heap_caps_aligned_alloc(128, src_bytes, MALLOC_CAP_SPIRAM);
    if (!src) { RG_LOGW("PPA-FRAME: 源缓冲申请失败，跳过\n"); return; }
    memset(src, 0, src_bytes);
    RG_LOGI("PPA-FRAME: src=%p (低7位=%u)  fb=%p (低7位=%u)\n",
            src, (unsigned)((uintptr_t)src & 127), tab5_fb, (unsigned)((uintptr_t)tab5_fb & 127));

    ppa_client_handle_t cli = NULL;
    ppa_client_config_t ccfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&ccfg, &cli) != ESP_OK || !cli)
    {
        RG_LOGW("PPA-FRAME: client 注册失败，跳过\n");
        heap_caps_free(src);
        return;
    }

    /* 三种 scale 组合各试一次 —— 现在源和目的都已 128 对齐，可以干净地判断 PPA 认哪个。
     * 前几轮测试都被"缓冲没对齐"污染过，那些结果不能作数。
     * 顺序：#0 = 1.0/1.0（纯旋转）；#1 = 交换(1280/720, 720/1280)；#2 = 不交换(720/1280, 1280/720)。 */
    static const float probe_sx[3] = {1.0f, (float)TAB5_PHYS_H / 720.0f, (float)TAB5_PHYS_W / 1280.0f};
    static const float probe_sy[3] = {1.0f, (float)TAB5_PHYS_W / 1280.0f, (float)TAB5_PHYS_H / 720.0f};
    for (int i = 0; i < 3; ++i)
    {
        ppa_srm_oper_config_t op = {0};
        op.in.buffer = src;
        op.in.pic_w = 1280; op.in.pic_h = 720;
        op.in.block_w = 1280; op.in.block_h = 720;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = tab5_fb;
        op.out.pic_w = TAB5_PHYS_W; op.out.pic_h = TAB5_PHYS_H;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
        /* ⚠ 旋转下 scale 的两个轴是**交换**的（照抄 R8T5：scale_x 用高度比、scale_y 用宽度比）。
         * 给 1.0/1.0 会被拒（ESP_ERR_INVALID_ARG 0x102）—— 因为那等于说"输出 1280x720"，
         * 而旋转后的输出实际是 720x1280。生产路径 mipi_dsi_tab5.h 里也是 1.0/1.0，同样的问题。 */
        op.scale_x = probe_sx[i];
        op.scale_y = probe_sy[i];
        op.out.block_offset_x = 0;
        op.out.block_offset_y = 0;

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = ppa_do_scale_rotate_mirror(cli, &op);
        int64_t dt = esp_timer_get_time() - t0;
        /* 只用整数格式化：ESP-IDF 的日志不支持 %lld / %f（用了会把后面的参数全部错位）。 */
        const int mbs10 = dt > 0 ? (int)(1840000 * 10 / dt) : 0;   /* 1.84MB / dt，×10 留一位小数 */
        RG_LOGI("PPA-FRAME: #%d 整帧270度 = %d us (输出 %d.%d MB/s) err=0x%x\n",
                i, (int)dt, mbs10 / 10, mbs10 % 10, (unsigned)err);
    }

    ppa_unregister_client(cli);
    heap_caps_free(src);
}

/* ── 电量圆灯（横屏）：写在**左下角留白**（用户 2026-10-08 指定："铭牌+圆灯放左下角"）──
 * 与竖屏那份（mipi_dsi_tab5_p.h 的 tab5_batt_led_refresh）只有三处不同：
 *   ① 像素索引走 90CW 映射（rg_batt_led_draw_cw90）—— 竖屏是"逻辑=物理"的线性写，
 *      横屏直接线性写会落到完全错误的位置（所以这条不是"挪坐标"，是必须换绘制通路）；
 *   ② 擦除区 = 灯的**逻辑矩形**映射到物理后的矩形（不是竖屏那种整行条带）；
 *   ③ 写回（msync）只覆盖那几条物理行 —— 行号按 4 取整，让偏移/长度都是 128 的整数倍
 *      （帧缓冲一行 1440B，128B 边界每 4 行落一次；同下面 FPS 数字那条带的处理）。
 * 灯是状态指示、**不是按键**：不参与命中判定，且落在所有机型最大游戏窗之外。 */
static void tab5_batt_led_refresh(void)
{
    if (!tab5_fb)
        return;
    static uint16_t *last_fb = NULL;
    static int64_t last_write = 0;
    const int64_t now = esp_timer_get_time();
    /* 换帧缓冲（进出游戏会重挂显示）或超过 5s 强制重画一次 —— 与竖屏同规则：
     * 否则"静止的灯"（纯绿）在应用切换后可能一直不重画。 */
    const bool forced = (tab5_fb != last_fb) || (now - last_write > 5000000);
    if (!rg_batt_led_refresh_needed() && !forced)
        return;
    last_fb = tab5_fb;
    last_write = now;

    /* 逻辑矩形 → 物理矩形：px = phys_w-1-ly、py = lx（与 rg_overlay_blit_cw90/全驱动同口径） */
    int lx0, ly0, lx1, ly1;
    rg_batt_led_get_rect(&lx0, &ly0, &lx1, &ly1);
    const int py0 = lx0 & ~3, py1 = (lx1 + 3) & ~3;
    const int px0 = TAB5_PHYS_W - ly1, px1 = TAB5_PHYS_W - ly0;
    if (py0 < 0 || py1 > TAB5_PHYS_H || px0 < 0 || px1 > TAB5_PHYS_W)
        return;                                  /* 越界就不画（几何写错时的兜底，不静默乱写） */

    /* 先擦成背景：**面板底是皮肤色不是黑** —— 用纯黑会在这块留白里割裂画面（竖屏同款教训）。 */
    const uint16_t bg = rg_batt_led_band_bg();
    for (int py = py0; py < py1; ++py)
    {
        uint16_t *row = tab5_fb + (size_t)py * TAB5_PHYS_W;
        for (int px = px0; px < px1; ++px)
            row[px] = bg;
    }
    rg_batt_led_draw_cw90(tab5_fb, TAB5_PHYS_W, TAB5_PHYS_W);

    const size_t off = (size_t)TAB5_PHYS_W * 2 * py0;
    const size_t len = (size_t)TAB5_PHYS_W * 2 * (py1 - py0);
    esp_cache_msync((void *)((uintptr_t)tab5_fb + off), len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void tab5_perf_report(void)
{
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* 电量圆灯跟着每秒的刷新判断（状态没变就是几次比较，可忽略）—— 与竖屏同一处位置。 */
    tab5_batt_led_refresh();
#endif
    /* 整帧 PPA 探针：只跑一次（面板已在扫描，拿的是真实条件下的数字）。
     * 2026-09-25 结论：三种 scale 变体全部 err=0x102（ESP_ERR_INVALID_ARG），
     * 即使源与目的都已 128 对齐 —— 说明还有别的原因，且**那个 9.4ms 不能采信**。
     * 探针已停用（它每次启动都往帧缓冲写整帧，会与面板扫描打架 → 真机实测 51~81 条
     * "previous draw operation is not finished" 报错）。留着代码备查。 */
#if 0
    static bool ppa_frame_probe_done = false;
    if (!ppa_frame_probe_done && tab5_fb)
    {
        ppa_frame_probe_done = true;
        tab5_ppa_frame_probe();
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
            " [xpose=%u.%02u ovl=%u.%02u draw=%u.%02u] rows=%u.%u max=%u"
            " gap_avg=%u us\n",
        (unsigned)(total / 1000), (unsigned)((total % 1000) / 10), (unsigned)(elapsed / 1000),
        (unsigned)(tab5_pf_tr_us / 1000), (unsigned)((tab5_pf_tr_us % 1000) / 10),
        (unsigned)(tab5_pf_sub_us / 1000), (unsigned)((tab5_pf_sub_us % 1000) / 10),
        (unsigned)tab5_pf_blocks,
        (unsigned)(tab5_pf_xp_us / 1000), (unsigned)((tab5_pf_xp_us % 1000) / 10),
        (unsigned)(tab5_pf_ov_us / 1000), (unsigned)((tab5_pf_ov_us % 1000) / 10),
        (unsigned)(tab5_pf_dr_us / 1000), (unsigned)((tab5_pf_dr_us % 1000) / 10),
        rows_avg10 / 10, rows_avg10 % 10, (unsigned)tab5_pf_rows_max,
        tab5_pf_blocks ? (unsigned)(tab5_pf_gap_us / tab5_pf_blocks) : 0);
#if RG_TAB5_PPA_MODE
    /* PPA 传输模式实验（2026-10-10）：把"提交侧 CPU 时间"与"完成延迟"分开打，按 op 平均。
     * 判读：sub_avg 大 ⇒ 成本在驱动/提交侧（cache msync、参数校验、入队）；
     *       wait_avg 大 ⇒ 成本在等待/硬件本体（模式 1 才有 wait；模式 2 的等待含在 sub 里）。
     * ⚠ ESP-IDF 日志不支持 %llu/%f ⇒ 全部先折成 unsigned。 */
    if (tab5_pf_ppa_ops)
    {
        const unsigned n = tab5_pf_ppa_ops;
        RG_LOGI("PPA: ops=%u sub_avg=%u us sub_max=%u us wait_avg=%u us wait_max=%u us"
                " ovl_blit=%u us ovl_sync=%u us stage=%u us stage_cp=%u us\n",
                n, (unsigned)(tab5_pf_ppa_sub_us / n), (unsigned)tab5_pf_ppa_sub_max,
                (unsigned)(tab5_pf_ppa_wait_us / n), (unsigned)tab5_pf_ppa_wait_max,
                (unsigned)(tab5_pf_ovl_blit_us / n), (unsigned)(tab5_pf_ovl_sync_us / n),
                (unsigned)(tab5_pf_stage_us / n), (unsigned)(tab5_pf_stage_cp_us / n));
    }
#endif
    /* 每秒把叠加层（屏幕帧率数字）直接合成进面板帧缓冲。
     * 为什么非这样不可：数字只有在"被推送的块正好覆盖它"时才会被重画，而游戏中的脏区
     * 极少覆盖到顶部正中 —— 真机表现就是"数字不刷新，必须点开 menu 才更新"（menu 走整屏推送）。
     * ⚠ 必须先擦除再画：这条路径不像正常推送那样把底下的游戏画面重画一遍，
     * 不擦的话新旧数字会叠在一起（真机已验证）。
     * 区域按 cw90 映射算：数字逻辑 y=44..76 → 物理 x=644..676（固定）；
     * 逻辑 x 随位数变化 → 物理 y 取一个够宽的带（1~3 位都盖得住）。
     * 黑底是刻意的：白字+黑投影本来就按深底设计，黑框还能让数字在亮场景里也看得清。 */
#if RG_OVERLAY_SHOW_FPS
    /* ⚠⚠ 这一整段是**开发调试专用**（屏幕帧率数字），必须被开关包住 ——
     * 2026-10-08 真机事故：发布版 `RG_OVERLAY_SHOW_FPS=0`，但这段"每秒清底 + 合成 + msync"
     * **没被包住**，于是每秒在画面里擦出一块黑矩形（物理 px640..680 / py584..696 → 经 cw90
     * 映射回画布 = x584..696 / y39..79），横屏下正好落在 **GB 4x 画面顶部正中** ⇒
     * 用户看到的现象是"画面正上方中间有个小黑长方形在闪烁"，而且永远没有数字补上（数字本身
     * 在开关后面）。顺带也省掉了每秒一次的**全叠加层合成 + 161KB msync**。 */
    if (tab5_fb)
    {
        const int cx0 = 640, cx1 = 680, cy0 = 584, cy1 = 696;   /* 物理坐标，比数字本身略大一圈 */
        for (int py = cy0; py < cy1; ++py)
        {
            uint16_t *row = tab5_fb + (size_t)py * TAB5_PHYS_W;
            for (int px = cx0; px < cx1; ++px)
                row[px] = 0x0000;
        }
        rg_overlay_blit_cw90(tab5_fb, TAB5_PHYS_W, 0, 0, TAB5_PHYS_W, TAB5_PHYS_H, TAB5_PHYS_W);
        /* 只写回数字所在的那条物理行带（y=584..696），不做整帧 1.84MB 写回 ——
         * 那是上次"蓝屏不断闪烁"的最大嫌疑。偏移 840960、长度 161280，都是 128 的整数倍。 */
        const size_t off = (size_t)TAB5_PHYS_W * 2 * 584;
        const size_t len = (size_t)TAB5_PHYS_W * 2 * 112;
        esp_cache_msync((void *)((uintptr_t)tab5_fb + off), len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
#endif  /* RG_OVERLAY_SHOW_FPS */

    tab5_pf_tr_us = tab5_pf_sub_us = 0;
    tab5_pf_xp_us = tab5_pf_ov_us = tab5_pf_dr_us = 0;
    tab5_pf_px = 0;
    tab5_pf_rows_max = 0;
    tab5_pf_blocks = 0;
#if RG_TAB5_PPA_MODE
    tab5_pf_ppa_sub_us = tab5_pf_ppa_wait_us = 0;
    tab5_pf_ppa_ops = tab5_pf_ppa_sub_max = tab5_pf_ppa_wait_max = 0;
    tab5_pf_ovl_blit_us = tab5_pf_ovl_sync_us = tab5_pf_stage_us = tab5_pf_stage_cp_us = 0;
    tab5_pf_gap_us = 0;
    tab5_pf_last_send_at = 0;
#endif
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
     * （上面那句 fprintf 是标准 stdio，所以那里用 %llu 是对的 —— 两者别混。）
     * 2026-10-08：本行原来写的是 %llu（竖屏线修过、横屏这份漏了），补上。 */
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

    /* 逻辑 ly = win_top + i, lx = win_left + j  ->
     * 物理 px = 719 - ly = (TAB5_PHYS_W - 1) - ly, py = lx
     * 于是 [(i,j) 块] 落在物理矩形 [x0, x0+rows) x [y0, y0+w)：
     *   x0 = TAB5_PHYS_W - win_top - rows,  y0 = win_left */
    const int x0 = TAB5_PHYS_W - tab5_win_top - rows;
    const int y0 = tab5_win_left;

    int64_t t_tr0 = esp_timer_get_time();
    /* 2026-10-10 P0：把「计时窗口外」的耗时也量出来 —— 相邻两次 send 的间隔 = 调用方
     * （模拟器核心 + rg_display + lcd_get_buffer）。教训：mode 1/D 两次都因为"窗口外没量"
     * 得出过反向结论（一次说 PPA 便宜、一次说 2fps 无法解释）。 */
    if (tab5_pf_last_send_at)
        tab5_pf_gap_us += (uint64_t)(t_tr0 - tab5_pf_last_send_at);
    tab5_pf_last_send_at = t_tr0;
    bool done = false;
    esp_err_t err = ESP_OK;
    const uint16_t *ppa_src = buffer;   /* mode 3：叠加层烘进暂存块后，源换成它 */
    bool ovl_in_src = false;            /* 叠加层是否已在源里 ⇒ 尾部就不该再写帧缓冲 */

    /* ---- 首选：PPA 硬件旋转（不做 CPU 转置、不产生 cache 写回）---- */
    if (tab5_ppa && tab5_fb && (((uintptr_t)buffer & 127u) == 0)) {
#if RG_TAB5_PPA_MODE == 1
        /* 收块点之二：本客户端 `max_pending_trans_num = 1` ⇒ 上一块没收掉就提交会被拒
         * （"exceed maximum pending transactions"）。等在这里 = 把"发生在提交点上的等待"
         * 换成"发生在下一块之前的等待"，这正是本次实验要量的事。 */
        tab5_ppa_drain();
#endif
#if RG_TAB5_PPA_MODE == 3
        /* ⛔⛔ 受试失败（2026-10-10）—— 这段代码**不要用**，除非先定位下面的问题：
         * 实测（dist/p0y-E3，240s，SPAM=0 干净日志）：显示路径本身是好的（稳态每块 0.91ms、
         * 叠加层混合只剩 44µs、换序拷贝 459µs），但**系统级崩塌** —— 240 秒里模拟器只推进了
         * 约 6 秒的量（DIAG_TEAR 心跳只有 6 行，对照基线与对照组都是 ~225/202 行）；无 panic、
         * 无 PPA 报错、无重启、无重启计数。
         * 已排除：UART/日志刷屏（SPAM=0 下同样复现）、PPA 输入对齐、`tab5_stage` 存储落点
         * （nm 实测 internal DRAM）、按键写入越界（blit 的裁剪逻辑有交点钳制）。
         * 已观察到的规律（未解释）：**"每块不再碰帧缓冲"这件事本身与崩塌强相关** ——
         * 当年的 D 组（跳过帧缓冲合成）同样掉到 ~2fps，两者唯一的共同点就是去掉了每块那两次
         * 帧缓冲 `esp_cache_msync`。⇒ 怀疑那两次 msync（或其带来的 cache 行为）是**承重的**，
         * 而不是可省的。**这是下次接手的人应该先验证的假设**（做法：保留 msync、只去掉像素混合，
         * 看是否仍然快 —— 若快，说明 msync 承重，则 P0 的"省 msync"这条路根本不存在）。
         * 结论：`RG_TAB5_PPA_MODE` 默认保持 0；本档仅在需要复现该异常时才编。 */
        /* ---- 原设计意图（失败，保留供复核）：叠加层合成到**本块源缓冲** ----
         * 为什么这样做（三条都是实测或构造推导，不是猜）：
         *  ① 旧写法把裁剪矩形传成整屏 ⇒ 每块都混合**全部 13 个按键**（≈13.6 万像素、在 PSRAM
         *     帧缓冲上读改写）= 30.4ms/块 —— 这才是「按块 PPA 慢 25 倍」的真身（C 组实测）。
         *  ② 想直接在帧缓冲上裁剪**做不到**：blit 把 (rx,ry) 当**缓冲原点**（传 (x0,y0) 会整体偏移，
         *     源码注释里记着这个坑）。
         *  ③ 合成到源缓冲则原点天然 = 本块 ⇒ 只混合本块内的按键，与 CPU 路径（合成到 tab5_scratch）
         *     同构；而且**完全不碰帧缓冲** ⇒ 没有 cache msync、不与 DSI/PPA 抢同一片 PSRAM。
         * 两个必须同时做的配套（只抄一半会颜色错/画面错）：
         *  · 源缓冲是**大端 565**（PPA 靠 `.byte_swap` 在输入侧换序，见 ppa.h:175），而叠加层
         *    `pal[]` 是**小端**（CPU 路径写的是 swap 过的 tab5_scratch）⇒ 暂存块要**换序拷贝**，
         *    并把 PPA 改成 `.byte_swap = false`（由 src_le 自动推导，不是写死）。
         *  · 块超过暂存缓冲（48KB）时**退回旧合成**（不静默丢按键），`ovl_in_src` 保持 false。
         * 落点等价性：blit_one 的 cw90 变换 `bx0 = phys_w - y - h` 与驱动的 `ly → px = (phys_w-1)-ly`
         * 是同一变换（rg_touch_overlay.c:1252 对 driver 注释），所以「逻辑合成 + PPA 旋转」与
         * 「物理合成」逐像素等价。 */
        {
            const int64_t t_s0 = esp_timer_get_time();
            const size_t blk_px = (size_t)w * (size_t)rows;
            if (blk_px * 2 <= TAB5_STAGE_BYTES)
            {
                for (size_t i = 0; i < blk_px; ++i)
                    tab5_stage[i] = tab5_swap16(buffer[i]);
                const int64_t t_s1 = esp_timer_get_time();
                /* 无 guard：与 538/807/1034 行同一写法（rg_touch_overlay.h 在关闭时给空实现） */
                rg_overlay_blit(tab5_stage, (int)w, tab5_win_left, tab5_win_top, (int)w, (int)rows);
                const int64_t t_s2 = esp_timer_get_time();
                tab5_pf_stage_cp_us += (uint64_t)(t_s1 - t_s0);
                tab5_pf_stage_us += (uint64_t)(t_s2 - t_s0);
                ovl_in_src = true;
                ppa_src = tab5_stage;
            }
            else
            {
                tab5_pf_stage_us += (uint64_t)(esp_timer_get_time() - t_s0);   /* 只记"判太大"这一步 */
            }
        }
#endif
        ppa_srm_oper_config_t cfg = {
            .in = {
                .buffer = ppa_src,
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
            .byte_swap = (ppa_src == buffer),   /* 源仍是"大端 565"才需要换序；mode 3 的暂存块已换序成小端 */
#if RG_TAB5_PPA_MODE == 1
            .mode = PPA_TRANS_MODE_NON_BLOCKING,   /* 实验：推入队列立刻返回，完成走中断回调 */
#else
            .mode = PPA_TRANS_MODE_BLOCKING,       /* 现状（当年 25x 那个形态）；模式 2 用它做对照 */
#endif
        };
        const int64_t t_ppa0 = esp_timer_get_time();
        err = ppa_do_scale_rotate_mirror(tab5_ppa, &cfg);
#if RG_TAB5_PPA_MODE
        {   /* 提交侧 CPU 时间（含驱动内部的 in/out cache msync）：与"完成延迟"分开记账 ——
             * 两者之比直接判定成本在**等待**还是在**驱动/硬件本体**（本次实验要回答的事）。 */
            const int64_t dt = esp_timer_get_time() - t_ppa0;
            tab5_pf_ppa_sub_us += (uint64_t)dt;
            tab5_pf_ppa_ops++;
            if (dt > (int64_t)tab5_pf_ppa_sub_max)
                tab5_pf_ppa_sub_max = (uint32_t)dt;
        }
#endif
        done = (err == ESP_OK);
        if (!done)
            RG_LOGW("PPA SRM failed (0x%x), falling back to CPU transpose\n", err);
#if RG_TAB5_PPA_MODE == 1
        else
        {
            /* 记下落点：叠加层合成推迟到收块之后（此刻 PPA 还在写这块帧缓冲） */
            tab5_ppa_pend.x0 = x0; tab5_ppa_pend.y0 = y0;
            tab5_ppa_pend.rows = rows; tab5_ppa_pend.w = w;
            tab5_ppa_sub_at = t_ppa0;
            tab5_ppa_inflight = true;
        }
#endif
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
         * 块超过 48KB 缓冲则自动退回原分块路径，行为与之前完全一致。 */
        const size_t tab5_blk_bytes = (size_t)rows * w * 2;
        const uint16_t *xsrc = buffer;
        if (tab5_blk_bytes <= TAB5_STAGE_BYTES) {
            memcpy(tab5_stage, buffer, tab5_blk_bytes);
            xsrc = tab5_stage;
        }
        #define TAB5_TR 32
        #define TAB5_TC 32
        for (int i0 = 0; i0 < rows; i0 += TAB5_TR) {
            const int ti = (rows - i0 < TAB5_TR) ? (rows - i0) : TAB5_TR;
            for (int j0 = 0; j0 < w; j0 += TAB5_TC) {
                const int tj = (w - j0 < TAB5_TC) ? (w - j0) : TAB5_TC;
                for (int ii = 0; ii < ti; ++ii) {
                    const int i = i0 + ii;
                    const uint16_t *src = xsrc + (size_t)i * w + j0;
                    const int a = rows - 1 - i;
                    uint16_t *dst = tab5_scratch + (size_t)j0 * rows + a;
                    for (int jj = 0; jj < tj; ++jj)
                        dst[(size_t)jj * rows] = tab5_swap16(src[jj]);
                }
            }
        }
        #undef TAB5_TR
        #undef TAB5_TC
        int64_t t_x1 = esp_timer_get_time();
        int64_t t_o1;
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
        /* 虚拟按键在推给面板前的最后一刻合成（避免被 GUI 立即模式的重绘覆盖 => 不闪） */
        rg_overlay_blit_cw90(tab5_scratch, rows, x0, y0, rows, w, TAB5_PHYS_W);
        t_o1 = esp_timer_get_time();
#else
        t_o1 = t_x1;
#endif
        err = tab5_draw(x0, y0, x0 + rows, y0 + w, tab5_scratch);
        /* DMA2D 忙时 IDF 会直接**丢弃**这次绘制（0 超时抢信号量 → ESP_ERR_INVALID_STATE），
         * 我们刚做完的 CPU 转置就白费了，这一帧也只能等下一帧脏区重推 ——
         * 实测每秒 317 次丢弃（≈ 推送次数的 51%），是纯浪费。
         * 有界重试：最多 5 次、每次让出 200µs（比 1ms 的 RTOS tick 细得多；
         * 用 ROM 忙等而非 vTaskDelay，既不进调度器也不会去抢总线）。
         * **必须有上限** —— 老笔记记的"队列深度 2 会楔死显示通路"就是缺限流的教训。 */
        for (int tries = 0; err == ESP_ERR_INVALID_STATE && tries < 5; ++tries) {
            esp_rom_delay_us(200);
            err = tab5_draw(x0, y0, x0 + rows, y0 + w, tab5_scratch);
        }
        int64_t t_d1 = esp_timer_get_time();
        if (err != ESP_OK)
            RG_LOGE("draw failed (err=0x%x) at phys <%d,%d %d,%d>\n", err, x0, y0, x0 + rows, y0 + w);
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
     * 实现与全部坑注都搬到了 tab5_ppa_compose_overlay()（2026-10-10 抽函数，逻辑一字未改）。
     * ⚠ 非阻塞形态（RG_TAB5_PPA_MODE == 1）下**不能在这里合成**：PPA 还在异步写这块帧缓冲，
     *   CPU 现在写同一片像素就是与硬件抢 ⇒ 推迟到收块之后（tab5_ppa_drain 里做）。 */
    if (done && !ovl_in_src)
    {
#if RG_TAB5_PPA_MODE == 1
        /* 故意留空：合成的唯一入口在 tab5_ppa_drain() */
#else
        tab5_ppa_compose_overlay(x0, y0, rows, w);
#endif
    }
#if RG_PPA_KEEP_FB_SYNC_EXP
    else if (done && ovl_in_src)
    {
        /* 实验（2026-10-10）：叠加层已在源缓冲里（mode 3）⇒ 不混像素，但仍按 mode 2 的形态
         * 补做这一步帧缓冲 cache 维护。与 E3（不补）只差这一个变量。 */
        tab5_ppa_fb_sync_only(x0, y0, rows, w);
    }
#endif
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
#if RG_TAB5_PPA_MODE == 1
    /* 收块点之三：一帧的最后一块也要收掉（否则它会一直"在飞"到下一帧的第一个块，
     * 那一块的叠加层也跟着晚一帧才出现）。rg_display 在每次写出流程结束时都会调到这里。 */
    tab5_ppa_drain();
#endif
    /* draw_bitmap 内部已做 cache writeback + 等 DMA；无需额外同步 */
}

static void lcd_set_rotation(int rotation)
{
    /* 旋转已固定在驱动的映射里（90° CW），此处不额外处理 */
    (void)rotation;
}
