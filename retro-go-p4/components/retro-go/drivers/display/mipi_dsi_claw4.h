/* ============================================================================
 * Metalio Claw4 (ESP32-P4 方屏 720×720) 显示驱动 — MIPI DSI / **NV3051F**
 * ----------------------------------------------------------------------------
 * 与 Tab5 驱动（drivers/display/mipi_dsi_tab5_p.h）的关系：
 *   **只复用"块推送"这条已验证路径**，tab5 那套专属机制一律不带过来 ——
 *     · 无 PPA 硬件旋转（本机不需要旋转）
 *     · 无 AXI-ICM QoS 调优 / 无 E0 带宽探针（那些是 tab5 抢带宽的产物，没证据不要搬）
 *     · 无 90° 转置映射（本机逻辑 = 物理、**线性 1:1**）
 *     · 无 M5Stack BSP 依赖（面板初始化走厂商序列 esp_lcd_nv3051f.c）
 *
 * 映射：逻辑 (lx, ly) -> 面板帧缓冲 (lx, ly)，帧缓冲行跨距 = 720px（无填充）。
 *   游戏区 240×160 @3x = 720×480 贴顶（y[0,480)），下方 240px 是虚拟手柄控制带；
 *   几何在 targets/claw4/geom.h，键位在 targets/claw4/touch_layout.h。
 *
 * 字节序：rg_display.c 对 RG_PIXEL_565_LE 源已交换过一次 ⇒ 到这里是 565 **大端**，
 *   DSI 面板要小端 ⇒ 逐像素换回（与 tab5 同一约定，见 claw4_swap16）。
 *
 * 关键坑（照 tab5 的经验，别踩第二遍）：
 *  - **背光必须在任何 set 之前初始化，且先置 0**（否则上电白闪）。
 *  - **必须调 RG_SCREEN_INIT()**（本 target 里是空宏，作为显式约定钩子；漏掉 = 显示驱动永不执行）。
 *  - esp_cache_msync 的**偏移与长度都必须是 128 字节的整数倍**，否则静默失败
 *    （720px 行 = 1440B，1440%128=32 ⇒ 直接拿窗口范围去 msync 一定踩坑，必须向外对齐）。
 *  - 不要自己写 esp_lcd_new_panel_dpi + DBI 初始化序列：交给厂商驱动，序列是厂里给的。
 *
 * 面板备选：厂商源码里另有 FL7707N（48MHz/RGB888）一套参数。若点不亮/花屏，
 *   第二个要试的就是它（另一套 DPI 时序 + 另一份 DCS 序列），不是先怀疑硬件。
 * 像素格式：厂商默认 24bpp（RGB888，93MB/s@60Hz）；我们走 **16bpp RGB565**
 *   （62MB/s，colmod 0x55）—— 由 vendor 驱动的 bits_per_pixel=16 自动配置。
 * ==========================================================================*/

#pragma once

#include <string.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"      /* esp_lcd_panel_io_tx_param：init 后改面板亮度 DCS */
#include "esp_lcd_mipi_dsi.h"      /* DPI 面板 + esp_lcd_dpi_panel_get_frame_buffer */
#include "esp_cache.h"             /* esp_cache_msync：写帧缓冲后必须写回 */
#include "esp_timer.h"             /* 诊断用一次性定时器（CLAW4_DIAG_MARKERS） */
#include "esp_ldo_regulator.h"     /* DSI PHY 供电（chan3 @2500mV） */
#include "freertos/FreeRTOS.h"  /* 测试用自动交接任务 */
#include "freertos/task.h"
#include "driver/ledc.h"           /* 背光 PWM */
#include "driver/i2c_master.h"     /* TCA9555（SD 卡槽供电使能） */
#include "esp_lcd_nv3051f.h"       /* 厂商面板驱动（组件根目录，第三方 vendor 代码） */
#include "esp_heap_caps.h"         /* 电量灯影子缓冲（PSRAM） */
#include "rg_touch_overlay.h"      /* 虚拟按键可视层（共享模块；本 target 已定义键位表） */

/* ──────────────────────────── 硬件常量（厂商 config.h / 板级源码）──────────── */
#define CLAW4_PHYS_W            720        /* 面板物理宽（= 逻辑宽，无旋转） */
#define CLAW4_PHYS_H            720
#define CLAW4_PIN_LCD_RST       GPIO_NUM_3
#define CLAW4_PIN_BACKLIGHT     GPIO_NUM_52   /* PWM，高电平有效（厂商：不反相） */
#define CLAW4_DSI_LDO_CHAN      3
#define CLAW4_DSI_LDO_MV        2500
#define CLAW4_DSI_LANES         2
#define CLAW4_DSI_LANE_MBPS     1000
/* DPI 像素时钟：厂商**运行中**的板级代码用 48MHz（面板头里的 60Hz 宏是 36MHz）。
 * 取 48 追随"实际能跑的配置"；若点屏异常，第一档就试 36。 */
#define CLAW4_DPI_CLK_MHZ       48
/* 像素格式（**这里踩过坑，改之前先读完**）：
 *   fb 侧 = RGB565（retro-go 的原生格式 ⇒ 推送路径不用做 CPU 转换，内存带宽减半）
 *   面板侧 = RGB888 / colmod 0x77（**跟厂商基线一致**）
 * 为什么面板侧不能是 16bpp：真机实测（2026-10-11）按 16bpp 配置时症状是
 *   「颜色错 + 画面被斜切成漏斗 + 整体发暗」—— 面板按 24bpp 解读 16bpp 流，
 *   每行像素数与时序都对不上，瞳孔里看到的就是这种斜切。
 *   ⇒ 面板侧回到厂商的 24bpp；**转换交给 DSI 桥的硬件色彩转换**（out_color_format），
 *     我们自己不写 565→888 的循环（那是每个像素的 CPU 税，P4 上白交）。
 * 若颜色变成 R/B 互换，改 dev_cfg.rgb_ele_order（一个开关，不用动别处）。 */
#define CLAW4_PANEL_BPP         24        /* → vendor 驱动据此写 colmod 0x77 */
#define CLAW4_FB_BPP            24        /* 帧缓冲也 888：与面板一致，转换自己做（见下） */
/* RGB888 帧缓冲的字节序：1 = B,G,R（IDF RGB888 帧缓冲 / LVGL RGB888 的内存布局，厂商同款）。
 * 真机若出现红蓝互换，改这里为 0 即可（只影响推送路径的 3 字节顺序）。 */
#define CLAW4_RGB888_BGR        1

/* 背光 PWM */
#define CLAW4_LEDC_TIMER        LEDC_TIMER_0
#define CLAW4_LEDC_CHANNEL      LEDC_CHANNEL_0
#define CLAW4_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define CLAW4_LEDC_DUTY_RES     LEDC_TIMER_10_BIT   /* 0..1023 */
#define CLAW4_LEDC_FREQ_HZ      5000

/* I2C（TCA9555：SD 卡槽供电 P0-3 必须拉低才使能；厂商 README） */
#define CLAW4_I2C_PORT          1
#define CLAW4_PIN_I2C_SDA       GPIO_NUM_7
#define CLAW4_PIN_I2C_SCL       GPIO_NUM_8
#define CLAW4_TCA9555_ADDR      0x20
#define CLAW4_TCA_OUT_PORT0     0x02        /* 输出寄存器 P0 */
#define CLAW4_TCA_CFG_PORT0     0x06        /* 方向寄存器 P0（0 = 输出） */

/* 面板显示开/关（DCS 0x29/0x28）。厂商板级代码里这行是**注释掉的**（= 不调），
 * 所以默认也跟随：置 1 试不通就说明该保持 0。 */
#define CLAW4_CALL_DISP_ON_OFF  0

/* ══════════════ 临时诊断（"界面不出来"排查；结论落地后删除）══════════════
 * 目的：一眼区分 —— ① launcher 的界面帧根本没进驱动  ② 进了但被跳过。
 * 两个视觉标记（都画在**保留带**里，不会被界面覆盖）：
 *   · 洋红方块（左下角 12x12）= 任何一次 lcd_send_buffer 调用
 *   · 青色方块（右上角 12x12）= **整宽**推送（宽 720 的行块 = 界面帧的特征）
 * 另：开机 3 秒后把整屏行校验和清零，逼 launcher 下一帧重推整屏
 *   （若界面因此出现 ⇒ 推送链路是通的，只是开机那几帧被跳过了）。 */
#define CLAW4_DIAG_MARKERS      1
#define CLAW4_DIAG_REPUSH_MS    3000

/* 面板亮度 / CABC（真机 2026-10-11 反馈"屏幕有点暗"的**真根因**，与我们的 PWM 无关）：
 * 厂商 DCS 序列里写着 `0x51 = 0x83`（≈51% 亮度）且 `0x55 = 0xD2`（开 CABC 内容自适应调光）——
 * 于是"软件 80% × 面板 51%"≈ 41%，暗是必然。
 * init 之后我们改回：0x51 = 0xFF（拉满）、0x55 = 0x00（关 CABC，暗场景不再被进一步压暗）。
 * 想用软件调光就改 CLAW4_DCS_BRIGHTNESS（0x00..0xFF）；设置里的背光档仍然只动 PWM。 */
#define CLAW4_DCS_BRIGHTNESS_FIX 0        /* 0 = 不动（**厂商原值**，官方 demo 就是这个值）
                                           * 1 = init 后改 0x51/0x55（见下） */
#define CLAW4_DCS_BRIGHTNESS    0xFF      /* 仅在 FIX=1 时使用 */
#define CLAW4_DCS_CABC_OFF      0x00

/* ⚠ PPA 转换档（P3 实验，2026-10-11）：
 *   动机：显示路径 = 帧的 79%，其中"往 PSRAM 帧缓冲写 888"实测 ~34 周期/像素；
 *         中间 565 缓冲在**片内 SRAM**（便宜）⇒ 融合省不了多少（A/B 已证），
 *         真正要挪走的是"逐像素写 PSRAM"这一步 ⇒ 交给 PPA 硬件 SRM 做格式转换。
 *   历史纠正：Tab5 那句"PPA 按块输 CPU 25×"的前提已被复测推翻 —— 真凶是每块跟着搬的
 *         叠加层合成（~30ms/块）；PPA 本体按块 0.41ms vs CPU 转置 1.06ms（快 2.6×）。
 *   失败/未注册 ⇒ 自动退回 CPU 展开（下面用 claw4_ppa != NULL 判）。 */
#ifndef CLAW4_PPA_CONVERT
#define CLAW4_PPA_CONVERT 1   /* ✓ 2026-10-15 复核后维持开启（真机取证，数据内联如下）：
                                       *  · v0.4.13 / MAC 30:ed:a0:e1:8c:5a，四组合颜色自检原始日志：
                                       *      rs=0 bs=0 maxdiff=248 | rs=1 bs=0 maxdiff=247
                                       *      rs=0 bs=1 maxdiff=  7 ✅唯一达标 | rs=1 bs=1 maxdiff=247
                                       *    ★选定 rgb_swap=0 byte_swap=1；真实块 claw4-ppa-real maxdiff=0 ≤ TOL(8)。
                                       *  · PPA 确实在加速：xpose=4ms/blocks=2；关掉后 xpose=53ms/blocks=25、
                                       *    msync 0->7，待机 FPS **106 -> 50**（同机对照）。故不关。
                                       *  · ⚠ 唯一待修（spike/claw4-ppa）：开机 y>=128 前几块
                                       *    `PPA convert failed: ESP_FAIL (w=720 rows=32)` x3 后落回 CPU，稳态正常。
                                       *    假设：H1 非阻塞流水线未排空即提交 / H2 块地址对齐阈值 /
                                       *    H4 "整宽 left=0" 守卫漏判。（H3 字节序已由上面的四组合矩阵排除）
                                       *  · 引用纪律：本仓库 .gitignore:57-67 有意不发布 SPEC/HANDOFF/VALIDATION
                                       *    类内部文档 ⇒ 代码注释**不要再引用它们的路径**（对公开仓库永远悬空）。
                                       *    可核验锚点用 commit sha 或 Release notes：v0.4.13 release。
                                       *  · 历史事故（仍有效，勿重犯）：BLOCKING 模式下"配置被接受但永不完成"会
                                       *    永久阻塞、不返回错误码 ⇒ 游戏里按 MENU（GUI 推 339/323 宽小块）整机卡死。
                                       *    若改动本通路：必须保持非阻塞 + 完成回调 + 有界等待
                                       *    (CLAW4_PPA_DRAIN_TIMEOUT_MS) + 超时永久停用，守卫只放整宽 left=0 的块。 */
#endif
/* ★ 这三个开关必须定义在所有使用点之前（2026-10-11 深夜踩过：开关定义在使用点之后 ⇒
 *   `#if CLAW4_PPA_NONBLOCK` 在未定义处按 0 求值 ⇒ 注册回调那段被**静默跳过**、
 *   且因为被跳过所以拿不到"未声明符号"的报错 ⇒ 编出来的镜像看着 BUILD=0、其实没有非阻塞）。
 *   与 skill 里"自报行只能引用定义点上方的宏"是同一条。 */
#ifndef CLAW4_PPA_NONBLOCK
#define CLAW4_PPA_NONBLOCK 1        /* 1 = 非阻塞+回调+有界等待（见下）；0 = 退回过阻塞式（危险） */
#endif
#ifndef CLAW4_PPA_DRAIN_TIMEOUT_MS
#define CLAW4_PPA_DRAIN_TIMEOUT_MS 2000   /* 有界等待上限；超时 ⇒ 永久停用该通路并落回 CPU */
#endif
#ifndef CLAW4_PPA_COLOR_TOL
#define CLAW4_PPA_COLOR_TOL 8       /* 逐通道门限：PPA 左移 vs CPU 位复制天然差 ≤7/255 */
#endif
#ifndef CLAW4_PPA_SCALE_TEST
#define CLAW4_PPA_SCALE_TEST 0   /* 测完改回 0 */
#endif
#ifndef CLAW4_PPA_RGB_SWAP
#define CLAW4_PPA_RGB_SWAP 0     /* A3 原始字节 dump 判：rs=1 会把 RED 变 0018e0（彻底错乱）；
                                   * rs=0/bs=1 才是通道正确（差 ≤7 = 左移 vs 位复制）。
                                   * 运行期颜色自检仍会自动定案并覆盖此默认值。 */
#ifndef CLAW4_PPA_BYTE_SWAP
#define CLAW4_PPA_BYTE_SWAP (-1)   /* -1 = 跟随 src_native（颜色自检会定案）；0/1 = 强制 */
#endif
#endif
#include "driver/ppa.h"
static ppa_client_handle_t claw4_ppa = NULL;

#if CLAW4_PPA_CONVERT
/* ── PPA 非阻塞形态（照 Tab5 mode-1，2026-10-11 深夜）──────────────────────────
 * 为什么是硬要求：PPA_TRANS_MODE_BLOCKING **一旦"配置被接受但永不完成"就永久阻塞、
 * 不返回错误码** ⇒ `if (ret != ESP_OK) 落回 CPU` 完全无效。真机实测：游戏里按 MENU
 * （GUI 推的 339/323 宽小块）⇒ 整机卡死，之后 63 秒全 0 帧、无 panic、无重启。
 * 门限 2s：照 Tab5 口径（200ms/1s 会在重载头几秒把 PPA 误关）。 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t claw4_ppa_done_sem = NULL;
static volatile bool     claw4_ppa_inflight  = false;
static int64_t           claw4_ppa_sub_at    = 0;

/* 颜色映射的运行期定案（自检在第一次推 PPA 时写入）。定义必须**早于** PERF 行（claw4_pf_report），
 * 那一行会打印它们 ⇒ 放在后面会 `undeclared`（本轮实测）。 */
static int  claw4_ppa_rgb_swap  = CLAW4_PPA_RGB_SWAP;
static int  claw4_ppa_byte_swap = CLAW4_PPA_BYTE_SWAP;   /* -1 = 跟随 src_native */

static void claw4_ppa_drain(void);   /* 定义在 PERF 计数器之后（要用到它们） */

static bool claw4_ppa_done_isr(ppa_client_handle_t cli, ppa_event_data_t *ev, void *ud)
{
    (void)cli; (void)ev; (void)ud;
    BaseType_t hp = pdFALSE;
    if (claw4_ppa_done_sem)
        xSemaphoreGiveFromISR(claw4_ppa_done_sem, &hp);
    return hp == pdTRUE;
}
#else   /* CLAW4_PPA_CONVERT == 0 —— 2026-10-15 补：此前关掉 PPA **编不过**（F7）
        * 因为 drain 前置声明与两个 PERF 变量都藏在这个 #if 里，而调用/打印点在守卫外
        * （:854 lcd_sync 调 drain、:912 static 定义连锁、:950 PERF 行打印 rgb/bsw）。
         * 桩只保证形态可编译与日志字段存在，不参与任何硬件动作。
         * ★ 教训：drain 只能给**前置声明** —— 真定义在 :919 且不在本守卫内（函数体自带
         *   #if CLAW4_PPA_CONVERT && CLAW4_PPA_NONBLOCK，关时天然是空函数）；
         *   在此再定义会撞 redefinition of 'claw4_ppa_drain'（本轮实测）。 */
static int  claw4_ppa_rgb_swap  = CLAW4_PPA_RGB_SWAP;   /* 仅供 PERF 行打印 */
static int  claw4_ppa_byte_swap = CLAW4_PPA_BYTE_SWAP;
static void claw4_ppa_drain(void);                       /* 定义见 :919，关时为空函数 */
#endif


static esp_lcd_panel_handle_t claw4_panel = NULL;
static esp_lcd_panel_io_handle_t claw4_io = NULL;   /* DBI io：init 之后还要发亮度 DCS */
/* DPI 帧缓冲（PSRAM，720×720 **RGB888**，行跨距 = 720*3 字节，无填充）。
 * 为什么 fb 也必须是 888：面板的 DSI 接收器按 24bpp 配置（厂商基线 colmod 0x77），
 * 而 IDF 的 dpi_panel_draw_bitmap **不做格式转换**（按 fb 的 bpp 直接 memcpy），
 * 所以"fb 565 + 流 888"只剩 DSI 桥的硬件转换一条路 —— 实测那条路是**蓝屏（欠载）**。
 * ⇒ fb 与流都 888，565→888 由本文件在推送路径里自己展开。 */
static uint8_t *claw4_fb = NULL;
static int claw4_fb_bytes = 0;
/* 收集逻辑行用；长度必须编译期 ⇒ 按 geom.h 的 LCD_BUFFER_LENGTH_MAX 分配（720×32）。
 * 64 字节对齐：msync 只按 128B 对齐切分，但源缓冲对齐能让 memcpy 走向量路径。 */
static uint16_t claw4_line_buffer[LCD_BUFFER_LENGTH_MAX] __attribute__((aligned(128)));
/* 叠加层合成暂存块（565 原生，**块内相对**索引）：rg_display 交来的是大端，
 * 共享叠加层模块只认原生 565 ⇒ 先还原进这里、画完按键再转 888。
 * 大小 = LCD_BUFFER_LENGTH_MAX（与 line_buffer 同量级，片内 RAM 合计约 92KB）。 */
static uint16_t claw4_ovl_scratch[LCD_BUFFER_LENGTH_MAX] __attribute__((aligned(128)));
static int claw4_win_left = 0, claw4_win_top = 0, claw4_win_width = 0;
static bool claw4_backlight_ready = false;

/* 565 → 面板 888 的一像素落盘。内存序 B,G,R 与 IDF/LVGL RGB888 一致（开关见 CLAW4_RGB888_BGR）。
 * **主写路径与电量灯路径共用这一份** —— 两处各写一遍必然漂移（红蓝互换只会在其中一处出现）。 */
static inline void claw4_put_pixel888(uint8_t *d, uint16_t v)
{
    const uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
    const uint8_t g6 = (uint8_t)((v >> 5) & 0x3F);
    const uint8_t b5 = (uint8_t)(v & 0x1F);
    const uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    const uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
    const uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
#if CLAW4_RGB888_BGR
    d[0] = b8; d[1] = g8; d[2] = r8;
#else
    d[0] = r8; d[1] = g8; d[2] = b8;
#endif
}

static inline uint16_t claw4_swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/* ═══ P0-A：4 像素一组的**字对齐**展开（565 → BGR888）════════════════════════════
 * 动机（claw4-PERF 真机实测，2026-10-11）：逐像素走 claw4_put_pixel888 = 往 PSRAM
 *   **一个像素 3 次单字节写**，实测 97ns/像素 ≈ 35 周期/像素、占整机墙钟 50%。
 *   同样像素按字写只要 ~15.6MB/s，而 PSRAM 顺序带宽实测 ~92MB/s ⇒ 成本在"逐字节"
 *   这个形态上，不在带宽上。
 * 做法：每 4 个源像素攒成 12 字节（= 3 个 32 位字，4 字节对齐才可用），一次写完；
 *   目的不对齐（win_left*3 % 4 != 0）或行尾不足 4 像素时，自动回落到老字节路径。
 * 语义必须与 claw4_put_pixel888 **逐字节一致** ⇒ 见 CLAW4_XPOSE_SELFCHECK（机器比对）。 */
#ifndef CLAW4_XPOSE_WORDS
/* ⚠ 2026-10-11 同场景 A/B 结论：**默认关**（保留开关只为复现负结果）。
 *   开：display=785 [copy=0 xpose=699]；关：display=791 [copy=148 xpose=555]
 *   ⇒ 两遍合并成一遍**没有收益**，总和不动（783 vs 784）。
 *   原诊断"逐像素 3 次单字节写 PSRAM = 35 周期/像素的元凶"被证伪：
 *   cache 本就把字节写聚合成整行写回，字宽不是瓶颈；真正的量级是**每帧搬运字节数**
 *   （2.5MB/帧 ÷ 24.7ms ≈ 101MB/s ≈ 本机 PSRAM 顺序带宽）⇒ 要走"少搬"路线（融合缩放+展开），
 *   不是"搬得更宽"。详见内部文档 CLAW4-P3-FPS（按 .gitignore 不随本仓库发布）。 */
#define CLAW4_XPOSE_WORDS 0
#endif
#ifndef CLAW4_XPOSE_SELFCHECK
#define CLAW4_XPOSE_SELFCHECK 1   /* spike/claw4-ppa：实验形态强制打开——与 CPU 路径逐字节比对。
                                        *   目的：为查 `PPA convert failed: ESP_FAIL (w=720 rows=32 y>=128)` x3 提供字节级证据；
                                        *   ⚠ 仅 spike 分支，勿合回主线（会拖帧）。 */
#endif

/* 允许别名访问的 32 位类型：按字写 PSRAM 时避免严格别名 UB。 */
typedef uint32_t __attribute__((may_alias)) claw4_u32a;

/* 单元格布局（小端）：B | G<<8 | R<<16，与 claw4_put_pixel888 的 d[0..2] 一一对应。 */
static inline uint32_t claw4_bgr(uint16_t v)
{
    const uint32_t r5 = (v >> 11) & 0x1Fu;
    const uint32_t g6 = (v >> 5) & 0x3Fu;
    const uint32_t b5 = v & 0x1Fu;
    const uint32_t r8 = (r5 << 3) | (r5 >> 2);
    const uint32_t g8 = (g6 << 2) | (g6 >> 4);
    const uint32_t b8 = (b5 << 3) | (b5 >> 2);
#if CLAW4_RGB888_BGR
    return b8 | (g8 << 8) | (r8 << 16);
#else
    return r8 | (g8 << 8) | (b8 << 16);
#endif
}

/* 4 个像素（已 native）→ 12 字节，3 个对齐字。 */
static inline void claw4_expand4(claw4_u32a *w, uint16_t v0, uint16_t v1, uint16_t v2, uint16_t v3)
{
    const uint32_t p0 = claw4_bgr(v0), p1 = claw4_bgr(v1), p2 = claw4_bgr(v2), p3 = claw4_bgr(v3);
    w[0] = (p0 & 0x00FFFFFFu) | ((p1 & 0xFFu) << 24);                       /* B0 G0 R0 B1 */
    w[1] = ((p1 >> 8) & 0xFFFFu) | ((p2 & 0x00FFu) << 16) | ((p2 & 0xFF00u) << 16); /* G1 R1 B2 G2 */
    w[2] = ((p2 >> 16) & 0xFFu) | ((p3 & 0x00FFu) << 8) | ((p3 & 0xFF00u) << 8) | ((p3 >> 16) << 24); /* R2 B3 G3 R3 */
}

/* 控制带（虚拟按键 + 皮肤面板）的顶边。claw4 touch_layout.h 的两套布局取 min：
 *   GBA 版按键 y[492,708]、SNES 版贴角 L/R y480..552 ⇒ 带顶 = 480。
 * 游戏区 240×160@3x 贴顶 = y0..480，**永不与带相交** ⇒ 那块「大端→原生拷贝 + 叠加层合成」
 * 是纯浪费（实测 copy 段 135ms/s ≈ 墙钟 13.5%）。Tab5 不走本驱动，此常量只影响 claw4。 */
#ifndef CLAW4_TEST_AUTOBOOT
#define CLAW4_TEST_AUTOBOOT 0   /* 测量构建：lcd_init 起任务，12s 后自动交接进游戏 */
#endif

#ifndef CLAW4_BAND_TOP
#define CLAW4_BAND_TOP 480
#endif

/* ──────────────────────────── 背光（LEDC PWM）──────────────────────────── */
static void claw4_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = CLAW4_LEDC_MODE,
        .duty_resolution = CLAW4_LEDC_DUTY_RES,
        .timer_num = CLAW4_LEDC_TIMER,
        .freq_hz = CLAW4_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK)
    {
        RG_LOGW("claw4: ledc timer config failed, backlight unavailable\n");
        return;
    }
    const ledc_channel_config_t ch = {
        .gpio_num = CLAW4_PIN_BACKLIGHT,
        .speed_mode = CLAW4_LEDC_MODE,
        .channel = CLAW4_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = CLAW4_LEDC_TIMER,
        .duty = 0,          /* ⚠ 先灭：任何 set 之前都不能亮，否则上电白闪 */
        .hpoint = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK)
    {
        RG_LOGW("claw4: ledc channel config failed\n");
        return;
    }
    claw4_backlight_ready = true;
}

/* ──────────────────────────── SD 卡槽供电（TCA9555 P0-3）──────────────────── */
/* 为什么放在显示驱动里做：厂商板级初始化顺序就是"I2C → 扩展器 → 屏"，而且
 * 卡槽供电不依赖显示，早做一步让 rg_storage 挂载时卡已经上电。
 * ⚠ **失败只告警**：本轮是点屏验证，SD 出问题不该把屏一起拖黑。 */
/* I2C 总线句柄（进程内唯一）：TCA9555 / 电量计 / GT911 触摸都挂在同一条总线上。
 * ⚠ 官方 demo 的板级注释：GT911 与 TCA9555 若各建各的 I2C 外设会**抢总线** ⇒ 通信失败。
 * 所以总线只在这里建一次，触摸初始化（rg_input.c）通过 claw4_get_i2c_bus() 复用。 */
static i2c_master_bus_handle_t claw4_i2c_bus = NULL;

i2c_master_bus_handle_t claw4_get_i2c_bus(void)
{
    if (claw4_i2c_bus)
        return claw4_i2c_bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CLAW4_I2C_PORT,
        .sda_io_num = CLAW4_PIN_I2C_SDA,
        .scl_io_num = CLAW4_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &claw4_i2c_bus) != ESP_OK)
        claw4_i2c_bus = NULL;
    return claw4_i2c_bus;
}

static void claw4_sd_power_enable(void)
{
    i2c_master_bus_handle_t bus = claw4_get_i2c_bus();
    if (!bus)
    {
        RG_LOGW("claw4: i2c bus init failed (SD 可能不可用；不影响显示)\n");
        return;
    }
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CLAW4_TCA9555_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK || !dev)
    {
        RG_LOGW("claw4: TCA9555 not found (SD 供电未知)\n");
        return;
    }
    /* 方向寄存器：P0 全输出（0）；输出寄存器：bit3 = 0（拉低使能 SD），其余保持高 */
    const uint8_t cfg_out[3] = {CLAW4_TCA_CFG_PORT0, 0x00, 0x00};
    const uint8_t out_val[3] = {CLAW4_TCA_OUT_PORT0, 0xF7, 0xFF};   /* bit3=0 ⇒ SD 使能 */
    esp_err_t e1 = i2c_master_transmit(dev, cfg_out, sizeof(cfg_out), 100);
    esp_err_t e2 = i2c_master_transmit(dev, out_val, sizeof(out_val), 100);
    RG_LOGI("claw4: TCA9555 SD power enable: cfg=0x%x out=0x%x\n", (int)e1, (int)e2);

#if CLAW4_PPA_CONVERT
    {
        ppa_client_config_t pc = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
            /* ★ 2026-10-11 夜补：Tab5 的同款注释记着 —— ".data_burst_length 我们原来完全没设 =
             * DMA 每次只搬一点点，和我们 CPU 转置踩的是同一个坑，只是这次踩在 DMA 引擎里；
             * 上次"PPA 慢 25 倍"很可能就是这一项造成的"。本仓同样漏了这一项。 */
            .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        };
        const esp_err_t pr = ppa_register_client(&pc, &claw4_ppa);
        RG_LOGI("claw4: PPA register: %s\n", esp_err_to_name(pr));
        if (pr != ESP_OK)
            claw4_ppa = NULL;   /* 失败则整档退回 CPU 展开 */
#if CLAW4_PPA_NONBLOCK
        if (claw4_ppa)
        {
            ppa_event_callbacks_t cbs = { .on_trans_done = claw4_ppa_done_isr };
            claw4_ppa_done_sem = xSemaphoreCreateBinary();
            if (!claw4_ppa_done_sem ||
                ppa_client_register_event_callbacks(claw4_ppa, &cbs) != ESP_OK)
            {
                RG_LOGW("claw4-ppa: 非阻塞回调注册失败 ⇒ 不用 PPA，全程 CPU\n");
                ppa_unregister_client(claw4_ppa);
                claw4_ppa = NULL;
            }
            else
                RG_LOGW("claw4-ppa: 非阻塞形态就绪（完成回调+信号量，超时 %dms）\n",
                        CLAW4_PPA_DRAIN_TIMEOUT_MS);
        }
#endif
    }
#endif

#if CLAW4_PPA_SCALE_TEST
    /* ── PPA 缩放滤波性质判定（机器可判）─────────────────────────────
     * 造 8×2 的测试图（每像素值互不相同，任何插值都会露馅），PPA 放大到 24×6，
     * 与 CPU 最近邻放大的结果逐字节比。不一致像素 = 0 ⇒ 是最近邻 ⇒ 可采用
     * "PPA 直接从模拟器原始小图放大进 fb" 的架构；否则只能做 1:1 格式转换。 */
    if (claw4_ppa)
    {
        /* PPA 硬要求缓冲 128 字节对齐：不对齐直接 ESP_ERR_INVALID_ARG，
         * 输出全 0，自检会误报成「插值」（上次就栽在这）。 */
        static uint16_t tsrc[8 * 2]  __attribute__((aligned(128)));
        static uint16_t tref[24 * 6] __attribute__((aligned(128)));
        static uint16_t tgot[24 * 8] __attribute__((aligned(128)));   /* 留一行余量 */
        for (int i = 0; i < 8 * 2; ++i)
            tsrc[i] = (uint16_t)(0x1000 + i * 0x0111);      /* 相邻像素必须不同 */
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 24; ++x)
                tref[y * 24 + x] = tsrc[(y / 3) * 8 + (x / 3)];   /* CPU 最近邻 3x */
        ppa_srm_oper_config_t tc = {
            .in  = { .buffer = tsrc, .pic_w = 8, .pic_h = 2,
                     .block_w = 8, .block_h = 2, .block_offset_x = 0, .block_offset_y = 0,
                     .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .out = { .buffer = tgot, .buffer_size = sizeof(tgot),
                     .pic_w = 24, .pic_h = 6,
                     .block_offset_x = 0, .block_offset_y = 0,
                     .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = 3.0f, .scale_y = 3.0f,
            .rgb_swap = false, .byte_swap = false,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        const esp_err_t tr = ppa_do_scale_rotate_mirror(claw4_ppa, &tc);
        int mism = 0, first = -1;
        for (int i = 0; i < 24 * 6; ++i)
            if (tgot[i] != tref[i]) { if (first < 0) first = i; mism++; }
        RG_LOGI("claw4-PPA-SCALE-TEST: err=%s mism=%d/%d first=%d ref[0]=%04x got[0]=%04x got[1]=%04x got[3]=%04x\n",
                esp_err_to_name(tr), mism, 24 * 6, first,
                tref[0], tgot[0], tgot[1], tgot[3]);
        RG_LOGI("claw4-PPA-SCALE-TEST: 判定 = %s\n",
                mism == 0 ? "最近邻（可采用 PPA 直接放大架构）" : "插值（像素画会糊 ⇒ 只做 1:1 格式转换）");
    }
#endif
}

/* ──────────────────────────── 面板初始化 ───────────────────────────────── */
/* 面板亮度修正：见 CLAW4_DCS_BRIGHTNESS 上方注释（厂商序列把亮度设成 51% 且开了 CABC）。 */
static void claw4_panel_brightness_fix(void)
{
#if !CLAW4_DCS_BRIGHTNESS_FIX
    return;                       /* 默认跟官方一致：面板亮度寄存器不动（0x51=0x83, 0x55=0xD2） */
#else
    if (!claw4_io)
        return;
    const uint8_t b = CLAW4_DCS_BRIGHTNESS;
    const uint8_t c = CLAW4_DCS_CABC_OFF;
    esp_err_t e1 = esp_lcd_panel_io_tx_param(claw4_io, 0x51, &b, 1);   /* Write Display Brightness */
    esp_err_t e2 = esp_lcd_panel_io_tx_param(claw4_io, 0x55, &c, 1);   /* Write CABC Control */
    RG_LOGI("claw4: panel brightness 0x51=0x%02X (was 0x83), CABC 0x55=0x%02X (was 0xD2) — tx err b=0x%x c=0x%x\n",
            b, c, (int)e1, (int)e2);
#endif
}

#if CLAW4_DIAG_MARKERS
/* 视觉探针：把 12x12 的方块画进**面板保留带**（界面与游戏都写不到那里）。
 * BGR 三字节序（CLAW4_RGB888_BGR=1）：洋红 = B=FF,G=00,R=FF；青 = B=FF,G=FF,R=00。 */
static uint32_t claw4_diag_calls, claw4_diag_fullw, claw4_diag_rows;
static int claw4_diag_last_x, claw4_diag_last_y, claw4_diag_last_w, claw4_diag_last_h;
static bool claw4_diag_after_boot;   /* 3 秒后的推送（= 界面帧是否还在来） */
static uint32_t claw4_diag_fullw_boot;   /* 3 秒前的整宽推送次数 */
static int claw4_diag_dump_blocks;       /* >0 = 还要逐块统计内容（强制重推时打开） */
static const bool claw4_diag_tag_pixels = false;  /* 画角标记（污染画面，只用字符照片验证时关掉） */
static uint32_t claw4_diag_widths[8];    /* 见过的块宽直方图（诊断用） */

static void claw4_diag_markers(bool full_width_push)
{
    if (!claw4_fb)
        return;
    claw4_diag_calls++;
    if (full_width_push)
    {
        claw4_diag_fullw++;
        if (!claw4_diag_after_boot)
            claw4_diag_fullw_boot++;
    }
    const int w = 12;
    for (int yy = 0; yy < w; ++yy)
    {
        for (int xx = 0; xx < w; ++xx)
        {
            uint8_t *m = claw4_fb + ((size_t)(CLAW4_PHYS_H - w + yy) * CLAW4_PHYS_W + xx) * 3;   /* 左下角 */
            m[0] = 0xFF; m[1] = 0x00; m[2] = 0xFF;                                               /* 洋红 */
            if (full_width_push)
            {
                uint8_t *c = claw4_fb + ((size_t)yy * CLAW4_PHYS_W + (CLAW4_PHYS_W - w + xx)) * 3; /* 右上角 */
                /* 时间编码：开机 3 秒内的整宽推送 = 蓝（B=FF,G=00,R=00）；
                 * 3 秒之后 = 青（B=FF,G=FF,R=00）—— 只有"青"才能说明界面帧**现在还在来**。 */
                c[0] = 0xFF; c[1] = claw4_diag_after_boot ? 0xFF : 0x00; c[2] = 0x00;
            }
        }
    }
    /* 两条带都要写回（128B 对齐：整行 2160B 不是 128 的整数倍，按整行范围向外扩） */
    esp_cache_msync((void *)((uintptr_t)claw4_fb), (size_t)w * CLAW4_PHYS_W * 3 + 128,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync((void *)((uintptr_t)claw4_fb + (size_t)(CLAW4_PHYS_H - w) * CLAW4_PHYS_W * 3),
                    (size_t)w * CLAW4_PHYS_W * 3 + 128, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

/* 3 秒后作废整屏行校验和：逼 launcher 下一帧重推整屏（只清数组，不跨任务重画界面）。 */
void rg_display_invalidate_lines(int top, int count);   /* 定义在 rg_display.c 后半（同 TU） */
static void claw4_diag_repush(void *arg)
{
    (void)arg;
    claw4_diag_after_boot = true;                 /* 之后的整宽推送会画成青色 */
    claw4_diag_dump_blocks = 20;                  /* 把接下来 20 块的内容统计出来 */
    rg_display_invalidate_lines(0, CLAW4_PHYS_H);
    RG_LOGI("claw4-diag: 3s 到 —— 行校验和已清零，且之后的整宽推送改画青色\n");
}

/* 把帧缓冲（= 面板此刻扫描的内容）打成 ASCII 字符画。
 * 用途：色块已证明"写进帧缓冲 = 面板显示"，所以这张字符画等价于给屏幕拍照，
 * 我不用问用户就能看清面板上到底是什么。
 * 每字符 = 8x12 像素，90 列 x 60 行覆盖 720x720。 */
static void claw4_diag_dump_fb(void)
{
    if (!claw4_fb)
        return;
    const int cw = 8, ch = 8;
    const int cols = CLAW4_PHYS_W / cw, rowsf = CLAW4_PHYS_H / ch;
    char lineb[160];
    RG_LOGI("claw4-diag-fb: 帧缓冲字符画（1 字符 = 8x12 px；' '=全黑 .=零星 :=少 +=中 #=多 @=很亮）\n");
    for (int cy = 0; cy < rowsf; ++cy)
    {
        int n = 0;
        if ((cy % 5) == 0 && cy > 0)
            RG_LOGI("fb|---- y=%d ----\n", cy * ch);
        for (int cx = 0; cx < cols; ++cx)
        {
            int cnt = 0;
            for (int y = 0; y < ch; ++y)
            {
                const uint8_t *p = claw4_fb + ((size_t)(cy * ch + y) * CLAW4_PHYS_W + cx * cw) * 3;
                for (int x = 0; x < cw; ++x, p += 3)
                    if (p[0] | p[1] | p[2]) cnt++;
            }
            const int pct = cnt * 100 / (cw * ch);
            lineb[n++] = cnt == 0 ? ' ' : pct < 5 ? '.' : pct < 25 ? ':' : pct < 50 ? '+' : pct < 75 ? '#' : '@';
        }
        lineb[n] = 0;
        RG_LOGI("fb|%s|\n", lineb);
    }
}

/* 每秒报一次：calls=总推送次数, fullw=整宽推送次数(其中 boot=3s前的), rows=累计行数, last=最后一块窗口 */
static void claw4_diag_report(void *arg)
{
    (void)arg;
    static int tick = 0;
    ++tick;
    if (tick == 6)                        /* 6 秒拍一张静态快照 */
        claw4_diag_dump_fb();
    RG_LOGI("claw4-diag: calls=%u fullw=%u boot_fullw=%u rows=%u after_boot=%d last=(%d,%d %dx%d)\n",
            (unsigned)claw4_diag_calls, (unsigned)claw4_diag_fullw, (unsigned)claw4_diag_fullw_boot,
            (unsigned)claw4_diag_rows, (int)claw4_diag_after_boot,
            claw4_diag_last_x, claw4_diag_last_y, claw4_diag_last_w, claw4_diag_last_h);
    RG_LOGI("claw4-diag-wh: 块宽直方图 = %u x%u, %u x%u, %u x%u, %u x%u\n",
            (unsigned)claw4_diag_widths[0], (unsigned)claw4_diag_widths[1],
            (unsigned)claw4_diag_widths[2], (unsigned)claw4_diag_widths[3],
            (unsigned)claw4_diag_widths[4], (unsigned)claw4_diag_widths[5],
            (unsigned)claw4_diag_widths[6], (unsigned)claw4_diag_widths[7]);
}
static esp_timer_handle_t claw4_diag_rep_timer = NULL;
static esp_timer_handle_t claw4_diag_timer = NULL;
#endif /* CLAW4_DIAG_MARKERS */

#ifndef RG_BATT_LED_ENABLE
/* 电量圆灯总开关（默认 1 = 原有行为）。claw4 在电量计（BQ27220）接上之前置 0：
 * 没数据会被当成 0% —— 既画错灯（红闪），又会把整条灯带擦成背景色 ⇒ 按键下方留一条黑带。 */
#define RG_BATT_LED_ENABLE 1
#endif

#if RG_OVERLAY_ENABLED && RG_BATT_LED_ENABLE
/* ── 电量圆灯（控制带里那颗）───────────────────────────────────────────────
 * 为什么必须由驱动直接画：灯在**控制带**（claw4 逻辑 y=692），而显示层只推视口（游戏区
 * y<480），叠加层与边框面板的推帧都刷不到它 —— 与 Tab5 的结论一致（见 rg_touch_overlay.h
 * 的 rg_batt_led_* 说明）。
 * 与 Tab5 的差别：Tab5 帧缓冲是 565，直接往 fb 上画；claw4 帧缓冲是 888 ⇒
 *   ① 灯画进一块 565 影子缓冲。高度按**绝对行号**开：rg_batt_led_draw() 内部用 (ly,lx)
 *      直接寻址，所以宁可多占 PSRAM，也不做"指针前移 by0 行"那种越界技巧（可读性优先）。
 *   ② 只把条带那几行 565→888 转进帧缓冲（复用主路径的 claw4_put_pixel888，防两处口径漂移）。 */
static uint16_t *claw4_led_shadow = NULL;
static esp_timer_handle_t claw4_led_timer = NULL;
static int64_t claw4_led_last_write = 0;
static uint8_t *claw4_led_last_fb = NULL;

/* 本块是否压到灯条带（灯在左下角、x 很窄，只判 y 足够） */
static bool claw4_led_overlaps(int top, int rows)
{
    int x0, y0, x1, y1;
    rg_batt_led_get_band(&x0, &y0, &x1, &y1);
    return (top < y1) && (top + rows > y0);
}

static void claw4_batt_led_refresh(bool force)
{
    if (!claw4_fb)
        return;
    const int64_t now = esp_timer_get_time();
    /* 换了帧缓冲（进出 app 会重挂显示）或超 5s 强制重画：否则"静止的灯"可能一直不刷新 */
    const bool stale = (claw4_fb != claw4_led_last_fb) || (now - claw4_led_last_write > 5000000);
    if (!force && !stale && !rg_batt_led_refresh_needed())
        return;
    claw4_led_last_fb = claw4_fb;
    claw4_led_last_write = now;

    int bx0, by0, bx1, by1;
    rg_batt_led_get_band(&bx0, &by0, &bx1, &by1);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > CLAW4_PHYS_W) bx1 = CLAW4_PHYS_W;
    if (by1 > CLAW4_PHYS_H) by1 = CLAW4_PHYS_H;
    if (bx0 >= bx1 || by0 >= by1)
        return;

    if (!claw4_led_shadow)
    {
        claw4_led_shadow = (uint16_t *)heap_caps_malloc((size_t)CLAW4_PHYS_H * CLAW4_PHYS_W * 2,
                                                        MALLOC_CAP_SPIRAM);
        if (!claw4_led_shadow)
        {
            RG_LOGW("claw4: 电量灯影子缓冲申请失败（灯不显示，其余不受影响）\n");
            return;
        }
    }

    /* 条带先擦成**皮肤底色**（不是纯黑：非黑面板上会割裂画面，Tab5 真机反馈过的坑） */
    const uint16_t bg = rg_batt_led_band_bg();
    for (int y = by0; y < by1; ++y)
    {
        uint16_t *row = claw4_led_shadow + (size_t)y * CLAW4_PHYS_W;
        for (int x = bx0; x < bx1; ++x)
            row[x] = bg;
    }
    rg_batt_led_draw(claw4_led_shadow, CLAW4_PHYS_W);   /* 线性映射：逻辑 = 面板 */

    for (int y = by0; y < by1; ++y)
    {
        const uint16_t *s = claw4_led_shadow + (size_t)y * CLAW4_PHYS_W + bx0;
        uint8_t *d = claw4_fb + ((size_t)y * CLAW4_PHYS_W + bx0) * 3;
        for (int x = 0, n = bx1 - bx0; x < n; ++x, d += 3)
            claw4_put_pixel888(d, s[x]);
    }
    /* 定点写回（msync 要求 128 对齐 ⇒ 向外扩） */
    const size_t off = ((size_t)by0 * CLAW4_PHYS_W + bx0) * 3;
    const size_t end = ((size_t)(by1 - 1) * CLAW4_PHYS_W + bx1) * 3;
    const size_t a0 = off & ~(size_t)127;
    size_t a1 = (end + 127) & ~(size_t)127;
    if (a1 > (size_t)claw4_fb_bytes) a1 = (size_t)claw4_fb_bytes;
    if (a1 > a0)
        esp_cache_msync((void *)((uintptr_t)claw4_fb + a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void claw4_led_timer_cb(void *arg)
{
    (void)arg;
    claw4_batt_led_refresh(false);
}
#endif /* RG_OVERLAY_ENABLED && RG_BATT_LED_ENABLE */

#if CLAW4_TEST_AUTOBOOT
static bool claw4_test_boot_started;
static void claw4_test_boot_task(void *arg)
{
    /* 2026-10-11：启动器里的自动进游戏钩子执行不到（控制流被吃掉），
     * 而 lcd_init 一定执行（自报行可证）⇒ 把交接挪到这里。
     * 先等 12s 让启动器和 SD/LDO 稳定，再走与钩子相同的 switch_app。
     * 守卫宏默认 0 ⇒ 可玩构建零影响（自报行会打印 AUTOBOOT）。 */
    /* 真实签名见 rg_system.h:219（第4参是 uint32_t）；不写 noreturn，保留"若返回"的诊断。 */
    extern void rg_system_switch_app(const char *part, const char *name, const char *args, uint32_t flags);
    vTaskDelay(pdMS_TO_TICKS(12000));
    const char *rom = "/sd/roms/gba/恶魔城_晓月之圆舞曲.gba";
    RG_LOGW("claw4-testboot: 交接 -> %s\n", rom);
    rg_system_switch_app("gbsp", "gba", rom, 0);
    RG_LOGW("claw4-testboot: switch_app 已返回（未重启？）\n");
    vTaskDelete(NULL);
}
#endif





static void lcd_init(void)
{
    claw4_backlight_init();
    lcd_set_backlight(0);            /* 明确先灭（init 期间不闪） */

    claw4_sd_power_enable();         /* 非致命 */

    /* DSI PHY 供电：P4 上 DSI 必须靠片上 LDO 供 PHY（chan3 @2500mV，厂商口径） */
    esp_ldo_channel_handle_t phy_ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = CLAW4_DSI_LDO_CHAN,
        .voltage_mv = CLAW4_DSI_LDO_MV,
    };
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &phy_ldo);
    if (err != ESP_OK)
        RG_LOGW("claw4: DSI PHY LDO acquire failed (0x%x)\n", (int)err);

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = NV3051F_PANEL_BUS_DSI_2CH_CONFIG();
    err = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus);
    if (err != ESP_OK)
    {
        RG_LOGE("claw4: esp_lcd_new_dsi_bus failed (0x%x)\n", (int)err);
        return;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = NV3051F_PANEL_IO_DBI_CONFIG();
    err = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io);
    if (err != ESP_OK)
    {
        RG_LOGE("claw4: esp_lcd_new_panel_io_dbi failed (0x%x)\n", (int)err);
        return;
    }

    /* 全链路 RGB888 = 官方 MetalioClaw4 板级代码的**逐字段同款**（main/boards/metalio-claw-4/
     * metalio-claw-4.cc: pixel_format/in/out 全 RGB888、48MHz、use_dma2d、colmod 24bpp）。
     * ⚠ 别退回 fb=RGB565 + out=RGB888 那条"让 DSI 桥做硬件转换"的路：
     *   IDF 的 dpi_panel_draw_bitmap **不做格式转换**（按 fb 的 bpp 直接 memcpy），
     *   实测那样是**蓝屏（DSI 欠载）**。fb 565 + 流 565（colmod 0x55）则是"颜色错 + 漏斗"，
     *   因为面板的 DSI 接收器是按 24bpp 配的，16bpp 流会让它的行时序逐行漂移。 */
    esp_lcd_dpi_panel_config_t dpi_cfg = NV3051F_720_720_PANEL_60HZ_DPI_CONFIG_CF(LCD_COLOR_FMT_RGB888);
    dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB888;   /* 与 fb 一致 = 不做桥转换（厂商基线） */
    dpi_cfg.num_fbs = 1;             /* 只一张帧缓冲：我们自己往里写，不要厂商那套三缓冲 */
    dpi_cfg.flags.use_dma2d = true;
    dpi_cfg.dpi_clock_freq_mhz = CLAW4_DPI_CLK_MHZ;   /* 覆盖宏里的 36（见文件头说明） */

    nv3051f_vendor_config_t vendor_cfg = {
        .init_cmds = NULL,           /* NULL = 用厂商默认 DCS 序列 */
        .init_cmds_size = 0,
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = CLAW4_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = CLAW4_PANEL_BPP,   /* 24 ⇒ colmod 0x77，与 DSI 流的 out_color_format 一致 */
        .vendor_config = &vendor_cfg,
    };
    err = esp_lcd_new_panel_nv3051f(io, &dev_cfg, &claw4_panel);
    if (err != ESP_OK || !claw4_panel)
    {
        RG_LOGE("claw4: esp_lcd_new_panel_nv3051f failed (0x%x)\n", (int)err);
        return;
    }
    claw4_io = io;
    esp_lcd_panel_reset(claw4_panel);
    esp_lcd_panel_init(claw4_panel);
    claw4_panel_brightness_fix();
#if CLAW4_CALL_DISP_ON_OFF
    esp_lcd_panel_disp_on_off(claw4_panel, true);
#endif

    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(claw4_panel, 1, &fb) == ESP_OK && fb)
    {
        claw4_fb = (uint8_t *)fb;
        claw4_fb_bytes = CLAW4_PHYS_W * CLAW4_PHYS_H * (CLAW4_FB_BPP / 8);
        memset(claw4_fb, 0, claw4_fb_bytes);          /* 先黑屏，避免看到上电垃圾数据 */
        esp_cache_msync(claw4_fb, claw4_fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    else
    {
        RG_LOGE("claw4: frame buffer unavailable\n");
    }

    claw4_win_left = claw4_win_top = 0;
    claw4_win_width = CLAW4_PHYS_W;

    RG_SCREEN_INIT();                /* 约定钩子（本 target 为空宏；漏掉=显示驱动永不执行） */

#if RG_OVERLAY_ENABLED
    /* 虚拟按键预渲染（一次性，约 100ms）：标签/边框/透明度形状在这一步烘成掩码，
     * 之后每帧只做查表混合。必须等显示起来后建（建层要读键位表/存 NVS）。 */
    rg_overlay_init();
#endif

    lcd_set_backlight(80);

#if RG_OVERLAY_ENABLED && RG_BATT_LED_ENABLE
    /* 电量灯刷新（4Hz）：样子没变时 rg_batt_led_refresh_needed() 直接返回 false，几乎零代价。 */
    if (!claw4_led_timer)
    {
        const esp_timer_create_args_t largs = {.callback = claw4_led_timer_cb, .name = "claw4-led"};
        if (esp_timer_create(&largs, &claw4_led_timer) == ESP_OK)
            esp_timer_start_periodic(claw4_led_timer, 250000);
    }
#endif

#if CLAW4_DIAG_MARKERS
    if (!claw4_diag_timer)
    {
        const esp_timer_create_args_t targs = {.callback = claw4_diag_repush, .name = "claw4-diag"};
        if (esp_timer_create(&targs, &claw4_diag_timer) == ESP_OK)
            esp_timer_start_once(claw4_diag_timer, (uint64_t)CLAW4_DIAG_REPUSH_MS * 1000);
    }
    if (!claw4_diag_rep_timer)
    {
        const esp_timer_create_args_t rargs = {.callback = claw4_diag_report, .name = "claw4-diag-rep"};
        if (esp_timer_create(&rargs, &claw4_diag_rep_timer) == ESP_OK)
            esp_timer_start_periodic(claw4_diag_rep_timer, 1000000);
    }
#endif
#if CLAW4_TEST_AUTOBOOT
    if (!claw4_test_boot_started) {
        claw4_test_boot_started = true;
        xTaskCreate(claw4_test_boot_task, "claw4boot", 4096, NULL, 5, NULL);
        RG_LOGW("claw4-testboot: 任务已起，12s 后自动进游戏\n");
    }
#endif
    RG_LOGW("claw4-build: PPA_CONVERT=%d SELFCHECK=%d SCALE_TEST=%d NUM_FBS=%d AUTOBOOT=%d AUTOPRESS=%d\n",
            (int)CLAW4_PPA_CONVERT, (int)CLAW4_XPOSE_SELFCHECK, (int)CLAW4_PPA_SCALE_TEST, dpi_cfg.num_fbs, (int)CLAW4_TEST_AUTOBOOT, (int)RG_TEST_AUTOPRESS_A_MS);
    RG_LOGI("claw4: DSI ready — NV3051F %dx%d, %d lanes @%dMbps, DPI %dMHz, RGB888, fb=%p\n",
            CLAW4_PHYS_W, CLAW4_PHYS_H, CLAW4_DSI_LANES, CLAW4_DSI_LANE_MBPS,
            CLAW4_DPI_CLK_MHZ, (void *)claw4_fb);
}

static void lcd_deinit(void)
{
    /* 常驻：本 target 不做 deinit（与 tab5 竖屏版一致）。 */
}

static void lcd_sync(void)
{
    claw4_ppa_drain();   /* 帧末收块 */
    /* 无待发送队列：lcd_send_buffer 是同步写帧缓冲 + msync，这里无事可做。
     * （面板扫描在后台按 DPI 时序持续读帧缓冲，写完即见。） */
}

static void lcd_set_rotation(int rotation)
{
    if (rotation)
        RG_LOGW("claw4: rotation %d ignored (方屏线性 1:1，不支持旋转)\n", rotation);
}

static void lcd_set_backlight(float percent)
{
    if (!claw4_backlight_ready)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    const uint32_t max_duty = (1u << CLAW4_LEDC_DUTY_RES) - 1u;
    const uint32_t duty = (uint32_t)(max_duty * (percent / 100.0f));
    ledc_set_duty(CLAW4_LEDC_MODE, CLAW4_LEDC_CHANNEL, duty);
    ledc_update_duty(CLAW4_LEDC_MODE, CLAW4_LEDC_CHANNEL);
    RG_LOGI("backlight set to %d%%\n", (int)percent);
}

static void lcd_set_window(int left, int top, int width, int height)
{
    (void)height;
    claw4_win_left = left;
    claw4_win_top = top;
    claw4_win_width = width;
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    (void)length;
    return claw4_line_buffer;
}


/* ═══ 临时性能探针（P3 帧率压榨，2026-10-11；量完按下面清单撤净）══════════════════
 * 照 mipi_dsi_tab5.h 的 PERF 格式：每秒一行，给 PC 侧统计（不看屏）。
 *   claw4-PERF: display=Tms/s [copy= ovl= xpose= msync= ] blocks=N rowsp1=N px=N
 *   display = 本驱动 lcd_send_buffer() 全程耗时；四段是它的组成部分（余量 = 记账外）。
 * 撤销清单（交付前逐条做）：
 *   1. 删本块（esp_timer.h include + claw4_pf_* 变量 + claw4_pf_report）
 *   2. lcd_send_buffer() 里 6 处时间戳/累计 + 末尾 1 处 claw4_pf_report() 调用
 *   3. 确认 esp_timer.h 在本文件别无他用（原本只有 rg_input.c 用） */
#include "esp_timer.h"
static uint64_t claw4_pf_total, claw4_pf_copy, claw4_pf_ovl, claw4_pf_xpose, claw4_pf_msync;
static uint32_t claw4_pf_blocks, claw4_pf_px, claw4_pf_selfcheck;
static uint64_t claw4_pf_ppa_us; static uint32_t claw4_pf_ppa_ops, claw4_pf_ppa_err;
static bool claw4_pf_msync_done;
static uint32_t claw4_pf_ppa_skip_log;
static int64_t claw4_pf_win = 0;
static uint64_t claw4_pf_ppa_wait = 0;

static void claw4_ppa_drain(void)
{
#if CLAW4_PPA_CONVERT && CLAW4_PPA_NONBLOCK
    if (!claw4_ppa_inflight)
        return;
    claw4_ppa_inflight = false;
    if (!claw4_ppa_done_sem || !claw4_ppa)
        return;
    if (xSemaphoreTake(claw4_ppa_done_sem, pdMS_TO_TICKS(CLAW4_PPA_DRAIN_TIMEOUT_MS)) == pdTRUE)
    {
        const int64_t dt = esp_timer_get_time() - claw4_ppa_sub_at;
        claw4_pf_ppa_wait += (uint64_t)dt;
        claw4_pf_xpose    += (uint64_t)dt;   /* 计入 xpose：与 CPU 路径同口径可比 */
        return;
    }
    static int to;
    if (to++ < 8)
        RG_LOGE("claw4-ppa: done 超时(>%dms) ⇒ 永久停用 PPA、改 CPU 展开（挂死不返回错误码）\n",
                CLAW4_PPA_DRAIN_TIMEOUT_MS);
    ppa_unregister_client(claw4_ppa);
    claw4_ppa = NULL;
#endif
}
static void claw4_pf_report(void)
{
    const int64_t now = esp_timer_get_time();
    if (claw4_pf_win == 0) { claw4_pf_win = now; return; }
    const int64_t el = now - claw4_pf_win;
    if (el < 1000000) return;
    RG_LOGI("claw4-PERF: display=%ums/%ums [copy=%u ovl=%u xpose=%u msync=%u] blocks=%u px=%u\n",
            (unsigned)(claw4_pf_total / 1000), (unsigned)(el / 1000),
            (unsigned)(claw4_pf_copy / 1000), (unsigned)(claw4_pf_ovl / 1000),
            (unsigned)(claw4_pf_xpose / 1000), (unsigned)(claw4_pf_msync / 1000),
            (unsigned)claw4_pf_blocks, (unsigned)(claw4_pf_px / 1024));
    if (claw4_pf_ppa_ops || claw4_pf_ppa_err)
        RG_LOGI("claw4-PERF-ppa: ops=%u err=%u us=%u wait=%u nb=%d rgb=%d bsw=%d\n",
                (unsigned)claw4_pf_ppa_ops, (unsigned)claw4_pf_ppa_err, (unsigned)claw4_pf_ppa_us,
                (unsigned)(claw4_pf_ppa_wait / 1000), (int)CLAW4_PPA_NONBLOCK,
                (int)claw4_ppa_rgb_swap, (int)claw4_ppa_byte_swap);
    claw4_pf_ppa_ops = claw4_pf_ppa_err = 0; claw4_pf_ppa_us = 0;
    claw4_pf_total = claw4_pf_copy = claw4_pf_ovl = claw4_pf_xpose = claw4_pf_msync = 0;
    claw4_pf_blocks = claw4_pf_px = 0;
    claw4_pf_win = now;
}

/* 把一块**逻辑行**写进面板帧缓冲：逻辑 (lx,ly) = 物理 (lx,ly) ⇒ 直接按行拷贝。
 * length 单位是像素（= 宽度 × 行数），与 rg_display.c 的约定一致；length=0 = 空操作。 */
#if CLAW4_PPA_CONVERT
/* ══════════════════════════════════════════════════════════════════════════
 * PPA 颜色映射自检（2026-10-11 深夜 · 第二版）
 *
 * v1 为什么是假绿：它抓**真实画面**的块与 CPU 参考逐字节比，结果抓到**纯黑块** ⇒
 *   4 种 (rgb_swap, byte_swap) 组合全部 bad=0 ⇒ "全部通过" ⇒ 门禁永远不会红 = 不是门禁。
 * v2 改法：用**合成测试图**，像素通道差异刻意做大（黑/灰像素对通道交换不敏感，判不出），
 *   并且按**真源的内存序**构造（src_native=false 时存大端交换后的值）⇒ 连 byte_swap 一起判。
 * 判定：只有逐字节全等的组合才算胜者；**找不到胜者就自动停用 PPA**（颜色正确优先于帧率）。
 * ══════════════════════════════════════════════════════════════════════════ */
static bool claw4_ppa_cfg_ready  = false;
static int  claw4_ppa_color_bad[4] = {-1, -1, -1, -1};    /* 供 PERF 行/脚本核对 */

/*
 * 颜色判据的门限（2026-10-11 深夜定，A3 原始字节 dump 实测）：
 * PPA 的 565→888 是**左移**（r5<<3），CPU 是**位复制**（(r5<<3)|(r5>>2)）
 *   ⇒ 两者**天然**差 ≤7/255（实测 maxdiff=7：RED 0000ff→0000f8、WHT ffffff→f8fcf8）。
 * 这不是错、肉眼不可辨。之前判"逐字节相等"⇒ 把颜色其实正确的组合也判死（假红）。
 * 实测唯一通道正确的组合是 rs=0/bs=1（A3；rs=1 会把 RED 变成 0018e0 = 彻底错乱）。
 * 想恢复严格模式：改成 0。
 */
static bool claw4_ppa_color_selftest(bool src_native)
{
    enum { CW = 32, CH = 8 };
    static uint16_t csrc[CW * CH]       __attribute__((aligned(128)));
    static uint8_t  cref[CW * CH * 3]   __attribute__((aligned(128)));
    static uint8_t  cout[CW * CH * 3]   __attribute__((aligned(128)));
    static const char *PERM[6] = { "RGB", "RBG", "GRB", "GBR", "BRG", "BGR" };
    static const int   PI[6][3] = { {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0} };

    uint32_t seed = 0x9e3779b9u;
    for (int i2 = 0; i2 < CW * CH; ++i2)
    {
        seed = seed * 1103515245u + 12345u;
        const uint16_t r = (uint16_t)((seed >> 16) & 0x1Fu);
        const uint16_t g = (uint16_t)((seed >> 8) & 0x3Fu);
        const uint16_t b = (uint16_t)(seed & 0x1Fu);
        const uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
        csrc[i2] = src_native ? v : claw4_swap16(v);
    }
    for (int i2 = 0; i2 < CW * CH; ++i2)
        claw4_put_pixel888(cref + i2 * 3, src_native ? csrc[i2] : claw4_swap16(csrc[i2]));

    int winner = -1;
    for (int ci = 0; ci < 4; ++ci)
    {
        const int rs = ci & 1, bs = (ci >> 1) & 1;
        memset(cout, 0x11, sizeof(cout));
        ppa_srm_oper_config_t c = {
            .in  = { .buffer = csrc, .pic_w = CW, .pic_h = CH, .block_w = CW, .block_h = CH,
                     .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .out = { .buffer = cout, .buffer_size = (uint32_t)sizeof(cout),
                     .pic_w = CW, .pic_h = CH, .block_offset_x = 0, .block_offset_y = 0,
                     .srm_cm = PPA_SRM_COLOR_MODE_RGB888 },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = 1.0f, .scale_y = 1.0f,
            .mirror_x = false, .mirror_y = false,
            .rgb_swap = (bool)rs, .byte_swap = (bool)bs,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        const esp_err_t e = ppa_do_scale_rotate_mirror(claw4_ppa, &c);
        if (e != ESP_OK)
        {
            RG_LOGW("claw4-ppa-color: rs=%d bs=%d CALL_FAIL %s\n", rs, bs, esp_err_to_name(e));
            continue;
        }
        /* 同序差（像素按同一下标、通道按同一序比） */
        int md_id = 0;
        for (int i2 = 0; i2 < CW * CH; ++i2)
            for (int ch = 0; ch < 3; ++ch)
            {
                const int df = (int)cref[i2 * 3 + ch] - (int)cout[i2 * 3 + ch];
                if (df > md_id) md_id = df;
                if (-df > md_id) md_id = -df;
            }
        /* 6 种通道排列里最接近的（用来抓"红蓝互换"这类真错：那时排列才最接近） */
        int md_best = 1 << 20, bi = 0;
        for (int pi = 0; pi < 6; ++pi)
        {
            int md = 0;
            for (int i2 = 0; i2 < CW * CH && md <= CLAW4_PPA_COLOR_TOL; ++i2)
                for (int ch = 0; ch < 3; ++ch)
                {
                    const int df = (int)cref[i2 * 3 + ch] - (int)cout[i2 * 3 + PI[pi][ch]];
                    if (df > md) md = df;
                    if (-df > md) md = -df;
                }
            if (md < md_best) { md_best = md; bi = pi; }
        }
        claw4_ppa_color_bad[ci] = md_id;
        RG_LOGW("claw4-ppa-color: rs=%d bs=%d maxdiff=%d bestperm=%s(%d)\n",
                rs, bs, md_id, PERM[bi], md_best);
        /* 判据：① 同序差 ≤ TOL（PPA 左移 vs CPU 位复制天然差 ≤7，不算错）
         *      ② 同序必须不劣于任何通道排列（⇒ 红蓝互换/通道错乱会被抓出来） */
        if (md_id <= CLAW4_PPA_COLOR_TOL && md_id <= md_best && winner < 0)
            winner = ci;
    }
    if (winner < 0)
        return false;
    claw4_ppa_rgb_swap  = winner & 1;
    claw4_ppa_byte_swap = (winner >> 1) & 1;
    RG_LOGW("claw4-ppa-color: ★选定 rgb_swap=%d byte_swap=%d（合成图 maxdiff≤%d 且同序最优）\n",
            claw4_ppa_rgb_swap, claw4_ppa_byte_swap, CLAW4_PPA_COLOR_TOL);
    return true;
}

/* 真实块端到端比对：拿**真实游戏源码**跑 CPU 展开与 PPA 展开各一遍，取最大通道差。
 * 这是颜色的终极机器判据（比合成图更硬）：真实数据、真实映射、真几何。
 * 只在第一次确定映射时跑一次；用已证被接受的 32×8 几何；BLOCKING 一次性调用。 */
static int claw4_ppa_real_verify(const uint16_t *src, bool src_native, int w)
{
    enum { RW = 32, RH = 8 };
    static uint16_t rv_in[RW * RH]     __attribute__((aligned(128)));
    static uint8_t  rv_out[RW * RH * 3] __attribute__((aligned(128)));
    static uint8_t  rv_ref[RW * RH * 3] __attribute__((aligned(64)));

    for (int r = 0; r < RH; ++r)
        for (int c = 0; c < RW; ++c)
        {
            const uint16_t v = src[(size_t)r * w + c];
            rv_in[r * RW + c] = v;
            claw4_put_pixel888(rv_ref + (r * RW + c) * 3, src_native ? v : claw4_swap16(v));
        }
    memset(rv_out, 0x11, sizeof(rv_out));
    ppa_srm_oper_config_t c = {
        .in  = { .buffer = rv_in, .pic_w = RW, .pic_h = RH, .block_w = RW, .block_h = RH,
                 .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = rv_out, .buffer_size = (uint32_t)sizeof(rv_out),
                 .pic_w = RW, .pic_h = RH, .block_offset_x = 0, .block_offset_y = 0,
                 .srm_cm = PPA_SRM_COLOR_MODE_RGB888 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = (bool)claw4_ppa_rgb_swap,
        .byte_swap = (claw4_ppa_byte_swap < 0) ? !src_native : (bool)claw4_ppa_byte_swap,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    const esp_err_t e = ppa_do_scale_rotate_mirror(claw4_ppa, &c);
    if (e != ESP_OK)
    {
        RG_LOGE("claw4-ppa-real: CALL_FAIL %s\n", esp_err_to_name(e));
        return -1;
    }
    int md = 0, nd = 0;
    for (int i2 = 0; i2 < RW * RH * 3; ++i2)
    {
        const int df = (int)rv_ref[i2] - (int)rv_out[i2];
        const int a = df < 0 ? -df : df;
        if (a > md) { md = a; nd = i2; }
    }
    RG_LOGW("claw4-ppa-real: maxdiff=%d (首个最差点 idx=%d ref=%02x%02x%02x got=%02x%02x%02x)\n",
            md, nd, rv_ref[nd - nd % 3], rv_ref[nd - nd % 3 + 1], rv_ref[nd - nd % 3 + 2],
            rv_out[nd - nd % 3], rv_out[nd - nd % 3 + 1], rv_out[nd - nd % 3 + 2]);
    return md;
}
/*
 * 颜色判据的门限见上方 CLAW4_PPA_COLOR_TOL（在自检函数之前定义）。
 */

#endif

static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (!claw4_fb || !buffer || !length || claw4_win_width <= 0)
        return;

    const int rows = (int)(length / (size_t)claw4_win_width);
    if (rows <= 0)
        return;
#if CLAW4_DIAG_MARKERS
    claw4_diag_last_x = claw4_win_left; claw4_diag_last_y = claw4_win_top;
    claw4_diag_last_w = claw4_win_width; claw4_diag_last_h = rows;
    claw4_diag_rows += (uint32_t)rows;
    {
        int slot = -1;
        for (int i = 0; i < 8; ++i)
            if (claw4_diag_widths[i * 2] == (uint32_t)claw4_win_width) { slot = i; break; }
            else if (claw4_diag_widths[i * 2] == 0) { slot = i; break; }
        if (slot >= 0) { claw4_diag_widths[slot * 2] = (uint32_t)claw4_win_width; claw4_diag_widths[slot * 2 + 1]++; }
    }
    if (claw4_diag_dump_blocks > 0)
    {
        /* 统计本块的内容：非黑像素数 + 非黑区域包围盒（源是 565 大端，黑 = 0x0000） */
        uint32_t nonblack = 0;
        int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
        for (int r = 0; r < rows; ++r)
        {
            const uint16_t *row = buffer + (size_t)r * claw4_win_width;
            for (int x = 0; x < claw4_win_width; ++x)
            {
                if (row[x] != 0)
                {
                    nonblack++;
                    if (x < bx0) bx0 = x;
                    if (x > bx1) bx1 = x;
                    if (r < by0) by0 = r;
                    if (r > by1) by1 = r;
                }
            }
        }
        claw4_diag_dump_blocks--;
        RG_LOGI("claw4-diag-blk: y=%d w=%d rows=%d nonblack=%u bbox=(%d,%d)-(%d,%d)\n",
                claw4_win_top, claw4_win_width, rows, (unsigned)nonblack, bx0, by0, bx1, by1);
    }
#endif
    if (claw4_win_left < 0 || claw4_win_left + claw4_win_width > CLAW4_PHYS_W ||
        claw4_win_top < 0 || claw4_win_top + rows > CLAW4_PHYS_H)
    {
        RG_LOGW("claw4: drop block out of panel: x=%d y=%d w=%d rows=%d\n",
                claw4_win_left, claw4_win_top, claw4_win_width, rows);
        return;
    }

    claw4_ppa_drain();   /* 入口先收上一块（非阻塞下 PPA 可能还在读源/暂存块） */
    const int64_t pf_all0 = esp_timer_get_time();

    /* 源是 565 大端（rg_display 已交换）→ 展开成 BGR888 写进面板帧缓冲。
     * 字节序 B,G,R：与 IDF RGB888 帧缓冲/LVGL RGB888 的内存布局一致（厂商同款）。
     * 若真机出现红蓝互换 ⇒ 把 CLAW4_RGB888_BGR 改 0（一处开关，别改别处）。
     * 顺序写整行（目标是 PSRAM，随机访问慢），最后整体 msync。 */
    /* ── 虚拟按键叠加层：必须在**推给面板前**合成 ──────────────────────────────
     * 为什么放在显示层：GUI 是立即模式，按键画在 GUI 层会被下一次重绘盖掉（实机表现为闪烁）。
     * 与 Tab5 的差别：共享模块只认 565 原生，而 rg_display 交来的是 565 **大端**
     *   （Tab5 是 565 帧缓冲，driver 里换个字节序直接画；claw4 是 888）。
     * 做法：本块先还原成 565 原生进暂存块 → 叠加层画在暂存块上 → 再整体转 888。
     * 索引是**块内相对**的（与 Tab5 用 tab5_stage 的调用方式一致）。 */
    const uint16_t *src_block = buffer;
    bool src_native = false;
    bool pf_ovl_ran = false;
    int64_t pf_t0 = esp_timer_get_time();
    /* P0-B：只有与控制带相交、且叠加层可见的块才需要"还原成原生 565 + 合成按键"。 */
#ifndef CLAW4_SKIP_NONBAND
#define CLAW4_SKIP_NONBAND 1     /* 0 = A/B 对照：恢复"每块都做拷贝+合成" */
#endif
#if CLAW4_SKIP_NONBAND
    /* 用叠加层**实际包围盒**判，而不是"块是否落在控制带内"：
     *   · 开关（眼睛）画在游戏区左上角 (12,12) —— 按"带"判会把游戏区的块全跳过，把开关擦掉；
     *   · 依据：blit 对每个元素都先做矩形裁剪 ⇒ **包围盒之外它一定是 no-op** ⇒ 跳过不改像素。 */
    const bool band_block = rg_overlay_rows_dirty(claw4_win_top, claw4_win_top + rows);
#else
    const bool band_block = true;
#endif
#if RG_OVERLAY_ENABLED
    if (band_block && rg_overlay_get_visible() &&
        (size_t)rows * (size_t)claw4_win_width <= (size_t)LCD_BUFFER_LENGTH_MAX)
    {
        pf_ovl_ran = true;
        for (int r = 0; r < rows; ++r)
        {
            const uint16_t *s = buffer + (size_t)r * claw4_win_width;
            uint16_t *d = claw4_ovl_scratch + (size_t)r * claw4_win_width;
            for (int x = 0; x < claw4_win_width; ++x)
                d[x] = claw4_swap16(s[x]);
        }
        int64_t pf_t1 = esp_timer_get_time();
        claw4_pf_copy += (uint64_t)(pf_t1 - pf_t0);
        rg_overlay_blit_linear(claw4_ovl_scratch, claw4_win_width, claw4_win_left, claw4_win_top,
                               claw4_win_width, rows, CLAW4_PHYS_W);
        claw4_pf_ovl += (uint64_t)(esp_timer_get_time() - pf_t1);
        src_block = claw4_ovl_scratch;
        src_native = true;
    }
#endif
    pf_t0 = esp_timer_get_time();

    uint8_t *dst = claw4_fb + ((size_t)claw4_win_top * CLAW4_PHYS_W + claw4_win_left) * 3;
    const bool dst_aligned = (((uintptr_t)dst & 3u) == 0);
#if CLAW4_PPA_CONVERT
    bool claw4_ppa_done = false;     /* 只有 PPA 真的成功才置位；失败必须落回 CPU 展开 */
#else
    const bool claw4_ppa_done = false;
#endif
#if CLAW4_PPA_CONVERT
    /* PPA 只处理"本块不需要叠加层合成"的情形（需要合成的块必须 CPU 逐像素画按键）。
     * 源必须已换序成 native（PPA 用 byte_swap 处理大端在真机上未被验证过 ⇒ 不冒险）。 */
    /* 自证：条件不满足必须说明原因，杜绝「看着跑了其实没跑」 */
    if (claw4_ppa == NULL && claw4_pf_ppa_skip_log < 3)
    {
        claw4_pf_ppa_skip_log++;
        RG_LOGE("claw4-ppa-skip: client=NULL（注册失败或未初始化）\n");
    }
    else if (claw4_ppa && (((uintptr_t)src_block & 127u) != 0) && claw4_pf_ppa_skip_log < 3)
    {
        claw4_pf_ppa_skip_log++;
        RG_LOGE("claw4-ppa-skip: 源未 128 对齐 mod128=%u\n", (unsigned)((uintptr_t)src_block & 127u));
    }
    else if (claw4_ppa && (((uintptr_t)claw4_fb & 127u) != 0) && claw4_pf_ppa_skip_log < 3)
    {
        claw4_pf_ppa_skip_log++;
        RG_LOGE("claw4-ppa-skip: 帧缓冲未 128 对齐 mod128=%u\n", (unsigned)((uintptr_t)claw4_fb & 127u));
    }
    else if (claw4_ppa && (size_t)rows * (size_t)claw4_win_width > (size_t)LCD_BUFFER_LENGTH_MAX && claw4_pf_ppa_skip_log < 3)
    {
        claw4_pf_ppa_skip_log++;
        RG_LOGE("claw4-ppa-skip: 块 %ux%u 超行缓冲上限 %d\n", (unsigned)rows, (unsigned)claw4_win_width, (int)LCD_BUFFER_LENGTH_MAX);
    }
    extern bool rg_system_is_launcher(void);   /* 启动器 UI 必须由 CPU 绘制（PPA 会写坏它）*/
    /* ★ 守卫收紧（2026-10-11 夜）：只处理**整宽、left=0** 的块。
     * 依据：卡死前最后一帧的块宽直方图出现 `339 x2, 323 x1` —— 那是 GUI/菜单推的小块。 */
    if (claw4_ppa && !band_block && !rg_system_is_launcher() &&
        claw4_win_width == CLAW4_PHYS_W && claw4_win_left == 0 &&
        (size_t)rows * (size_t)claw4_win_width <= (size_t)LCD_BUFFER_LENGTH_MAX &&
        ((uintptr_t)src_block & 127u) == 0 && ((uintptr_t)claw4_fb & 127u) == 0)
    {
        /* 第一次真的要推 PPA 时定映射：用**真实这一块的 src_native**跑合成图自检。
         * 失败（无任何组合逐字节一致）⇒ 永久停用 PPA，并让本块也落回 CPU。 */
        if (!claw4_ppa_cfg_ready)
        {
            claw4_ppa_cfg_ready = true;
            bool color_ok = claw4_ppa_color_selftest(src_native);
            if (color_ok && claw4_win_width >= 32 && rows >= 8)
            {
                const int rmd = claw4_ppa_real_verify(src_block, src_native, claw4_win_width);
                if (rmd < 0 || rmd > CLAW4_PPA_COLOR_TOL)
                {
                    RG_LOGE("claw4-ppa-real: 真实块 maxdiff=%d > %d ⇒ 判定颜色不可用，停用 PPA\n",
                            rmd, CLAW4_PPA_COLOR_TOL);
                    color_ok = false;
                }
                else
                {
                    RG_LOGW("claw4-ppa-real: ✓ 真实块颜色一致（maxdiff=%d ≤ %d）\n", rmd, CLAW4_PPA_COLOR_TOL);
                }
            }
            if (!color_ok)
            {
                RG_LOGE("claw4-ppa-color: 颜色判据不过 ⇒ 停用 PPA（颜色正确优先于帧率）\n");
                ppa_unregister_client(claw4_ppa);
                claw4_ppa = NULL;
            }
        }
        if (!claw4_ppa)
            goto claw4_ppa_disabled;
        ppa_srm_oper_config_t pcfg = {
            .in = {
                .buffer = (void *)src_block,
                .pic_w = (uint32_t)claw4_win_width, .pic_h = (uint32_t)rows,
                /* ★ 必须给 block_w/block_h：缺省=0 ⇒ PPA 读到 0×0 块 ⇒ 挂死（不是报错）。
                 * 依据：本文件里已通过的 8x2 自检就是这么写的（.pic_w=8,.pic_h=2,.block_w=8,.block_h=2）。 */
                .block_w = (uint32_t)claw4_win_width, .block_h = (uint32_t)rows,
                .block_offset_x = 0, .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = (void *)claw4_fb,
                .buffer_size = (uint32_t)claw4_fb_bytes,     /* ★ 漏给这条 = err 0x102 */
                .pic_w = (uint32_t)CLAW4_PHYS_W, .pic_h = (uint32_t)CLAW4_PHYS_H,
                .block_offset_x = (uint32_t)claw4_win_left, .block_offset_y = (uint32_t)claw4_win_top,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = 1.0f, .scale_y = 1.0f,
            .mirror_x = false, .mirror_y = false,
            .rgb_swap = (bool)claw4_ppa_rgb_swap,
            .byte_swap = (claw4_ppa_byte_swap < 0) ? !src_native : (bool)claw4_ppa_byte_swap,
            .mode = CLAW4_PPA_NONBLOCK ? PPA_TRANS_MODE_NON_BLOCKING : PPA_TRANS_MODE_BLOCKING,
        };
        /* PPA 绕过 CPU cache 直写 PSRAM ⇒ 先失效目的区间，否则 CPU 里的陈旧行稍后回写会覆盖它 */
        {
            const size_t o0 = (size_t)((uintptr_t)dst - (uintptr_t)claw4_fb) & ~(size_t)127;
            const size_t o1 = ((size_t)((uintptr_t)dst - (uintptr_t)claw4_fb) +
                               (size_t)(rows - 1) * CLAW4_PHYS_W * 3 +
                               (size_t)claw4_win_width * 3 + 127) & ~(size_t)127;
            const size_t oe = o1 > (size_t)claw4_fb_bytes ? (size_t)claw4_fb_bytes : o1;
            if (oe > o0)
                esp_cache_msync((void *)((uintptr_t)claw4_fb + o0), oe - o0, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }
        const int64_t t_ppa0 = esp_timer_get_time();
        const esp_err_t perr = ppa_do_scale_rotate_mirror(claw4_ppa, &pcfg);
        const int64_t dt_ppa = esp_timer_get_time() - t_ppa0;
        claw4_pf_xpose += (uint64_t)dt_ppa;                 /* 记进 xpose 段：与 CPU 版可比 */
        claw4_pf_ppa_us += (uint64_t)dt_ppa;
        claw4_pf_ppa_ops++;
        if (perr == ESP_OK)
        {
            claw4_pf_msync_done = true;
#if CLAW4_PPA_NONBLOCK
            claw4_ppa_inflight = true;
            claw4_ppa_sub_at = esp_timer_get_time();
#endif   /* PPA 直写 PSRAM，不经 CPU cache ⇒ 跳过 msync */
            claw4_ppa_done = true;
        }
        else
        {
            /* 失败 ⇒ 不置 done ⇒ 落到下面的 CPU 展开。绝不允许"记条日志就当画好了" */
            claw4_pf_ppa_err++;
            if (claw4_pf_ppa_err < 4)
                RG_LOGE("claw4: PPA convert failed: %s (w=%d rows=%d y=%d) — 落回 CPU\n",
                        esp_err_to_name(perr), claw4_win_width, rows, claw4_win_top);
        }
    }
#endif
#if CLAW4_PPA_CONVERT
claw4_ppa_disabled:   /* 自检判定无胜者 ⇒ PPA 已停用，本块也走 CPU */
#endif
    if (!claw4_ppa_done)
    {
    for (int r = 0; r < rows; ++r)
    {
        const uint16_t *src_row = src_block + (size_t)r * claw4_win_width;
        uint8_t *dst_row = dst + (size_t)r * CLAW4_PHYS_W * 3;
        int x = 0;
#if CLAW4_XPOSE_WORDS
        if (dst_aligned)
        {
            claw4_u32a *w = (claw4_u32a *)dst_row;
            for (; x + 4 <= claw4_win_width; x += 4)
            {
                const uint16_t a = src_native ? src_row[x]     : claw4_swap16(src_row[x]);
                const uint16_t b = src_native ? src_row[x + 1] : claw4_swap16(src_row[x + 1]);
                const uint16_t c = src_native ? src_row[x + 2] : claw4_swap16(src_row[x + 2]);
                const uint16_t e = src_native ? src_row[x + 3] : claw4_swap16(src_row[x + 3]);
                claw4_expand4(w, a, b, c, e);
                w += 3;
            }
        }
#endif
        for (; x < claw4_win_width; ++x)
            claw4_put_pixel888(dst_row + x * 3,
                               src_native ? src_row[x] : claw4_swap16(src_row[x]));
    }
#if CLAW4_XPOSE_SELFCHECK
    /* 新旧两条路径逐字节比对（前 8 个块 × 前 4 行 × 前 64 像素）——机器判红蓝互换。 */
    if (claw4_pf_selfcheck < 24)
    {
        uint32_t bad = 0;
        const int nw = claw4_win_width < 64 ? claw4_win_width : 64;
        const int nr = rows < 4 ? rows : 4;
        for (int r = 0; r < nr; ++r)
        {
            const uint16_t *src_row = src_block + (size_t)r * claw4_win_width;
            const uint8_t *dr = dst + (size_t)r * CLAW4_PHYS_W * 3;
            for (int x = 0; x < nw; ++x)
            {
                uint8_t ref[3];
                claw4_put_pixel888(ref, src_native ? src_row[x] : claw4_swap16(src_row[x]));
                if (ref[0] != dr[x * 3] || ref[1] != dr[x * 3 + 1] || ref[2] != dr[x * 3 + 2])
                    bad++;
            }
        }
        RG_LOGI("claw4-PERF: xpose selfcheck #%u bad=%u (w=%d rows=%d left=%d aligned=%d band=%d native=%d)\n",
                (unsigned)claw4_pf_selfcheck, (unsigned)bad, claw4_win_width, rows, claw4_win_left,
                (int)dst_aligned, (int)band_block, (int)src_native);
        claw4_pf_selfcheck++;
    }
#endif
    claw4_pf_xpose += (uint64_t)(esp_timer_get_time() - pf_t0);
    pf_t0 = esp_timer_get_time();
    }   /* ← CPU 展开/回退块结束 */
    if (claw4_pf_msync_done) { claw4_pf_msync_done = false; goto claw4_skip_msync; }

    /* cache 写回：msync 要求偏移与长度都是 **128 字节的整数倍**，否则静默失败。
     * 窗口偏移不保证对齐（720px 行 = 1440B，1440%128=32）⇒ 向外扩到 128B 边界，
     * 多写几个字节无害（同一张帧缓冲、都是我们要显示的内容）。 */
    const size_t off = (size_t)((uintptr_t)dst - (uintptr_t)claw4_fb);
    const size_t px_bytes = CLAW4_FB_BPP / 8;
    const size_t stride = (size_t)CLAW4_PHYS_W * px_bytes;
    const size_t len = (size_t)claw4_win_width * px_bytes;
    size_t start = off & ~(size_t)127;
    size_t end = (off + (size_t)(rows - 1) * stride + len + 127) & ~(size_t)127;
    if (end > (size_t)claw4_fb_bytes)
        end = (size_t)claw4_fb_bytes;
    esp_cache_msync((void *)((uintptr_t)claw4_fb + start), end - start, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    claw4_pf_msync += (uint64_t)(esp_timer_get_time() - pf_t0);
claw4_skip_msync:;

#if CLAW4_DIAG_MARKERS
    if (claw4_diag_tag_pixels)
        claw4_diag_markers(claw4_win_width >= CLAW4_PHYS_W);
#endif
#if RG_OVERLAY_ENABLED && RG_BATT_LED_ENABLE
    /* 本块压到电量灯条带上 ⇒ 立刻把灯补画回去（否则被面板底图擦掉，要等下一拍定时器）。
     * 只在相交时做，正常游戏帧（游戏区 y<480）不付这个代价。 */
    if (claw4_led_overlaps(claw4_win_top, rows))
        claw4_batt_led_refresh(true);
#endif

    /* ★★ 连续推送时窗口向下推进 ★★
     * rg_display.c 的流式路径只在"窗口不连续"时才调 lcd_set_window（见 rg_display.c:450
     * 的 `if (top != window_top)` 记账）—— 它**假定驱动内部有写游标**会在每次推送后下移。
     * 漏掉这一行的后果：整屏每一块都写回同一个 win_top（我实测 dump 出 15 块全是 y=0），
     * 最后一块覆盖前面的 ⇒ launcher 界面永远出不来（只有直写路径的沙漏能看见）。
     * 语义与 Tab5 驱动 mipi_dsi_tab5.h:1165 一字不差，别删。 */
    claw4_win_top += rows;

    claw4_pf_total += (uint64_t)(esp_timer_get_time() - pf_all0);
    claw4_pf_blocks++;
    claw4_pf_px += (uint32_t)rows * (uint32_t)claw4_win_width;
    claw4_pf_report();
    (void)pf_ovl_ran;
}
