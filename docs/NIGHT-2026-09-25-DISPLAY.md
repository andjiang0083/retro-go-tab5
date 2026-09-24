# 夜间实验记录：显示通路瓶颈攻关（2026-09-25 夜）

> 用户睡前授权："利用我睡觉的时间，把显示效率的瓶颈解决"；并要求：找资料、仿真、自己安排计划。
> 本文件记录夜间每一步的**证据与结论**，供明早快速接手。所有改动都会 commit。

## 起点状态

- 设备已刷 `dist/retro-go-p2.6.4-ppa-revert.img`（回滚 PPA 后的 CPU 转置基线，回读校验一致）
- 已知现象：GBA 满屏负载下 ~15fps；显示通路每秒 431ms（占整机 43% CPU）；日志刷
  `E lcd.dsi.dpi: dpi_panel_draw_bitmap(530): previous draw operation is not finished`

## 证据一：15fps 的机制 = 队列深度 1 把两条流水线串行化（代码级确认）

`rg_display.c:649` 用 `RG_DISPLAY_QUEUE_LEN = 1` 建显示队列，而 `rg_system.c` 里
`rg_task_send()` 是 `xQueueSend(task->queue, msg, portMAX_DELAY)` —— **队列满时无限阻塞**。

于是模拟器线程每次提交都要等显示任务把上一帧画完，两条流水线被串行化。
驱动注释里记着早年实测："深度提到 2 能让真正画出来的帧数从 15/秒 翻到 30/秒"，但会**楔死**
（画面定格、只能断电恢复）；当时结论是"要再试必须配限流方案 + 运行时开关"。

## 证据二：厂商 BSP 的 DPI 配置 —— 我们走的正是 DMA2D 异步路径

`vendor/m5stack_tab5/m5stack_tab5.c`（我们的驱动就是调它的 `bsp_display_new_with_handles_to_st7123`）：

```c
.num_fbs          = 1,
.pixel_format     = LCD_COLOR_PIXEL_FORMAT_RGB565,
.dpi_clock_freq_mhz = 60,
.flags.use_dma2d  = true,     // ← 开启 DMA2D 硬件异步拷贝
```

## 证据三：IDF 源码里那条报错的真实语义 —— **丢帧，不是重试**

`$IDF_PATH/components/esp_lcd/dsi/esp_lcd_panel_dpi.c`（v5.5.4，行 545-571）：

```c
} else { // copy by DMA2D
    // ensure the previous draw operation is finished
    ESP_RETURN_ON_FALSE(xSemaphoreTake(dpi_panel->draw_sem, 0) == pdTRUE, ESP_ERR_INVALID_STATE,
                        TAG, "previous draw operation is not finished");
    ...
    esp_async_fbcpy_trans_desc_t ...   // 异步硬件搬运
}
```

**0 超时抢信号量**：上一次 DMA2D 还没搬完 → 直接返回错误 → **这次绘制被丢弃**（`draw_bitmap`
返回非 OK，而我们的驱动只在 `err != ESP_OK` 时打一行日志，画面这块就停在旧内容）。

**推论：日志里每秒 317 条报错 = 每秒 317 次绘制被硬件队列拒收丢弃。**

## 证据四（仿真的价值）：Mac 侧微基准**证伪**了"跨步写放大"假设

`tools/bench_transpose.c` 对比当前驱动的跨步转置 vs 分块转置（含逐字节正确性校验）：

| 方案 | ns/像素 | 相对 |
|---|---|---|
| A 跨步（当前驱动实现） | 0.26 | 1.00x |
| B 分块 16×16 | 0.30 | **0.89x（更慢）** |
| C 分块但不做字节交换 | 0.29 | 0.90x |

- 现代 CPU（Mac）有**跨步预取器**，跨步写并不会造成 cache line 放大；分块反而打乱访问顺序
- 字节交换几乎免费
- **结论：CPU 转置在"访存模式"层面没有可优化的空间**，原假设被证伪（省下了一轮无效真机实验）

## 尚未定论：那 0.6~1.0ms/次到底花在哪

算账：每块 ~28 行 × 720 列 = 约 40KB；真机实测 0.6~1.0ms/次。两个候选：

1. **CPU 转置**：P4 是 400MHz 小核，若**没有跨步预取器**，11520 次跨步写 × ~25 周期 ≈ 0.6ms —— 数量级对得上；
   而 Mac 的 0.26ns/像素 ↔ 真机的 52ns/像素 相差 200 倍，说明真机上**要么转置确实贵、要么开销不在转置**。
2. **DMA 提交 / cache 同步 / 信号量**：`transpose=` 这一项把「转置循环 + 虚拟键合成 + `tab5_draw`」混在一起，**看不出是哪一段**。

→ **必须上真机细分打点**（本次实验内容）。

## 下一步（本轮实施）

1. 细分打点：把 `transpose=` 拆成 `xpose=`（纯转置循环）/ `ovl=`（虚拟键合成）/ `draw=`（tab5_draw）
   并统计平均每块行数（`px` / `blocks` / 宽 = 行数），确认分块是否真为 ~28 行
2. 编译 → 刷机 → 抓开机日志（该负载确定性好，已用于多轮 A/B）
3. 视结果选下一刀：
   - 若转置贵 → 换实现（无跨步的写法 / 让 DMA2D 承担字节序）
   - 若 `tab5_draw` 贵 → 查 msync 与信号量（并考虑"丢弃即丢帧"是否要改成等待+排队）
   - 若两者都贵 → 上"深度 2 + 限流"，把 15→30fps 那条已知路径安全地拿回来

---

## 证据五（决定性）：IDF 官方指路 **AXI-ICM**，而 P4 上真有这套寄存器

**来源**：`$IDF_PATH/components/esp_lcd/dsi/esp_lcd_panel_dpi.c:105-110`，DSI 欠载中断的处理：

```c
if (intr_status & MIPI_DSI_BRG_LL_EVENT_UNDERRUN) {
    // when an underrun happens, the LCD display may already becomes blue
    // it's too late to recover the display, so we just print an error message
    // as a hint to the user that he should optimize the memory bandwidth (with AXI-ICM)
    ESP_DRAM_LOGE(TAG, "can't fetch data from external memory fast enough, underrun happens");
}
```

**P4 上确有这套硬件与低层接口**：`$IDF_PATH/components/hal/esp32p4/include/hal/axi_icm_ll.h`（191 行，完整读了）

主设备表（原样摘录，这决定了该调谁）：

```c
AXI_ICM_MASTER_CPU        = 0   // HP/LP CPU、USB、EMAC、SDMMC、AHB-GDMA 等聚合口
AXI_ICM_MASTER_CACHE      = 1   // Cache 主端口  ← 我们 CPU 转置的写回走这里
AXI_ICM_MASTER_DW_GDMA_M0 = 5   // DW-GDMA 主端口 0 ← IDF 的 DPI 面板扫描链路走这里
AXI_ICM_MASTER_DW_GDMA_M1 = 6
AXI_ICM_MASTER_GDMA       = 8
AXI_ICM_MASTER_DMA2D      = 10  // DMA2D       ← 我们推送用的异步拷贝走这里
AXI_ICM_MASTER_H264_M0/M1 = 11/12
```

可用接口（都是 `static inline`，直接写寄存器）：

| 接口 | 作用 |
|---|---|
| `axi_icm_ll_set_qos_burstiness(mid, burstiness, access)` | **令牌桶突发深度**（1..256）——"不要整屏突发"的硬件版 |
| `axi_icm_ll_set_qos_peak_transaction_rate(mid, peak, xct, access)` | 峰值/事务速率限流（每 N 周期放一个 token） |
| `axi_icm_ll_set_dma2d_qos_arbiter_prio(w, r)` | DMA2D 读写 QoS |
| `axi_icm_ll_set_dw_gdma_qos_arbiter_prio(port, w, r)` | DW-GDMA（**面板扫描**）读写 QoS |
| `axi_icm_ll_set_cache_qos_arbiter_prio(w, r)` | **CPU cache 写回** QoS |
| `axi_icm_ll_set_cpu_qos_arbiter_prio(w, r)` | CPU 聚合口 QoS |

### 带宽账（把症状和数字对上）

- 面板：1280×720×2B = 1.84MB/帧；时序 (1280+2+40+40)×(720+2+8+220)=1362×950，
  `dpi_clock_freq_mhz = 70` ⇒ 帧率 70M/1.29M ≈ **54Hz** ⇒ 扫描带宽 ≈ **100MB/s 的持续 PSRAM 读**
- 我们：313 次推送/秒 × 40KB ≈ **12.5MB/s**，看起来微不足道
- 但实测每次推送 **0.6~1.0ms**（按 40KB 算只有 ~67MB/s 的有效带宽），DMA2D 还常年"忙"到
  51% 的绘制被拒收丢弃

⇒ **不是吞吐不够，是延迟被饿死**：视频主设备的长突发把总线占住，CPU cache 写回与 DMA2D 拷贝的
每次访问都要排队等。这同时解释了：PPA 直写为什么慢 25 倍（硬件引擎同样卡在 PSRAM 上）、
"深度 2 会楔死"为什么像总线饿死。

### 已实施的改动（保守版）

`lcd_init()` 里新增一段 AXI-ICM 调优，**只抬高我们自己的两条主设备**，不动面板：

- `axi_icm_ll_set_cache_qos_arbiter_prio(15, 15)` —— CPU 转置写回提权
- `axi_icm_ll_set_dma2d_qos_arbiter_prio(15, 15)` —— 推送拷贝提权
- 同时把两者的**默认 QoS 打日志**（`axi_icm_ll` 没有 getter，直接读 `AXI_ICM.mst_*qos_reg0` 结构体），
  这样第一天就能看到默认值、判断提权空间

**刻意不动面板的 QoS/突发限制**：给它降权或限突发有过冲导致 underrun（画面变蓝）的风险，
需要真机分档实测后再决定。

### 真机验证清单（设备插回来后）

1. 看日志 `AXI-ICM qos defaults: ...` 的默认值（判断是否真有提升空间）
2. 对比 PERF 行：`draw=` 段耗时、`blocks=`、以及 `previous draw operation is not finished` 条数
   （**丢弃条数是最直接的指标**，它代表有多少次绘制被硬件队列拒收）
3. 若提权有效 → 再试"给面板降突发"（`set_qos_burstiness(DW_GDMA_M0, N, READ)`）分档，
   同时盯 underrun 日志
4. 配合 `RG_DISPLAY_QUEUE_LEN = 2` 再测一轮，看能否安全拿回那个已知的 2 倍

