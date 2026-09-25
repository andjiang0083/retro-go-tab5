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

---

# 夜间进度状态（交接给明早）

## 设备状态：**未连接**

凌晨准备刷机时发现 `/dev/cu.usbmodem*` 不存在、USB 总线上也没有串口设备 ——
设备在睡前被拔掉（或线松了）。所以本轮的**所有真机 A/B 都没有执行**，
夜间工作转为"找资料 + 仿真 + 备好待验证改动"，与睡前约定的三条线一致。

## 已经做完并有据可查的

| 产出 | 位置 | 状态 |
|---|---|---|
| 15fps 机制归因（队列深度 1 串行化） | 本文件·证据一 | 代码级确认 ✓ |
| "报错=丢帧而非重试"结论 | 本文件·证据三 | IDF 源码确认 ✓ |
| 厂商 BSP 配置（RGB565 / dma2d / num_fbs=1 / 70MHz） | 本文件·证据二 | 已核对 ✓ |
| 转置访存假设被证伪 | `tools/bench_transpose.c` | 主机实测 ✓ |
| **AXI-ICM 是官方指路方向** | 本文件·证据五 | IDF 注释 + P4 寄存器确认 ✓ |
| 三段细分打点（xpose/ovl/draw + 每块行数） | `mipi_dsi_tab5.h` | 已编译进镜像 ✓ |
| AXI-ICM QoS 提权（cache/dma2d → 15，默认 0） | `mipi_dsi_tab5.h` | 已编译进镜像 ✓ |

**待刷镜像（明早的候选，按风险从低到高）**：

| 镜像 | 内容 | 风险 | 期望 |
|---|---|---|---|
| `dist/retro-go-p2.6.6-axi-icm.img` | 三段打点 + AXI-ICM QoS 提权 | 低（只是提权，不动面板） | 丢帧数下降、draw 段耗时下降 |
| `dist/retro-go-p2.6.7-retry-qos.img` | 以上 + DMA2D 忙时有界重试 | 低（上限 5×200µs，不会卡死） | 丢弃的绘制能被救回，减少白做的转置 |
| `dist/retro-go-p2.6.8-depth2.img` | 再 + 显示队列深度 1→2 | **中（老笔记记录过楔死）** | 若成功：帧数翻倍（15→30） |

**回退镜像**：`dist/retro-go-p2.6.4-ppa-revert.img`（已验证可跑的基线）

⚠ **建议顺序**：先刷 p2.6.6 看默认 QoS 值和丢帧计数的变化 → 有效再刷 p2.6.7 → 最后才试 p2.6.8。
p2.6.8 一旦出现画面定格：直接断电重启，然后刷回 p2.6.4 或 p2.6.7（不会有任何数据损坏）。

## 明早按顺序做（每步都有明确的判据）

**第 0 步**：把设备插回 USB（`ls /dev/cu.usbmodem*` 能看到即可）

**第 1 步：验证 AXI-ICM 提权**（最高优先级，改动最小、最可能见效）
```
刷 dist/retro-go-p2.6.6-axi-icm.img → 抓开机日志 → 看三件事
```
- 日志应出现 `AXI-ICM qos defaults: cache w/r=0/0 dma2d w/r=0/0`（默认值 = 0，证实有提权空间）
- PERF 行对比基线：`[xpose= ovl= draw=]` 三段耗时 —— 找出 0.6~1.0ms 到底在哪一段
- `previous draw operation is not finished` 条数：**这是丢帧计数**，若从 317/秒 明显下降即命中

**第 2 步（若第 1 步有效）**：给面板扫描降突发（`axi_icm_ll_set_qos_burstiness(AXI_ICM_MASTER_DW_GDMA_M0, N, AXI_ICM_ACCESS_READ)`，N 从 32 试到 8），
同时盯 `underrun happens`（画面变蓝的警报）—— 有欠载就回退到 N 更大。

**第 3 步（若前两步还不够）**：`RG_DISPLAY_QUEUE_LEN = 2` + **在 `tab5_draw` 失败时有界重试**
（现在 DMA2D 忙就直接丢帧，导致已做好的转置白费；改成最多重试 N 次再放弃并计数）。
⚠ 老笔记记着"深度 2 会楔死"，所以必须**同时**有重试上限和计数，且一旦画面定格立刻刷回基线。

## 下一步代码改动设计（尚未实施）

`lcd_send_buffer()` 里 `tab5_draw` 返回 `ESP_ERR_INVALID_STATE`（DMA2D 忙）时：

```c
int tries = 0;
while (err == ESP_ERR_INVALID_STATE && ++tries <= 5) {
    esp_rom_delay_us(200);          /* 让出总线，转速快于 1ms 的 tick */
    err = tab5_draw(x0, y0, x0 + rows, y0 + w, tab5_scratch);
}
if (err == ESP_ERR_INVALID_STATE) tab5_pf_drops++;   /* 计数，替代静默丢弃 */
```

## 仿真这条线做了什么、还剩什么

- **已做**：`tools/bench_transpose.c` —— 主机侧对**转置访存模式**做真机前仿真，
  **证伪**了"跨步写放大"假设（Mac 上有跨步预取器，分块反而更慢）。这一步的价值是
  **省下了一轮无效真机实验**。
- **未做**：队列/丢帧流水线的 Python 模型。原因：AXI-ICM 这个发现把优先级拉到了硬件 QoS 上，
  而该改动无法用模型验证（要真机看丢帧计数）。若第 1~3 步仍不达标，建议下轮先补这个模型。

---

# 补充（同一夜稍后）：仿真跑完了，出了两个重要发现

## 仿真结果（`tools/sim_display_pipeline.py`，3 秒 × 20µs 步长）

| 方案 | 更新/秒 | 块送达率 | 丢块/秒 | 主线程被卡 |
|---|---|---|---|---|
| ① 现状：深度 1 + 忙则丢弃 | 30.7 | **50.0%** | 625 | 96.9% |
| ② 只加有界重试（深度仍 1） | 34.3 | **100.0%** | 0 | 98.1% |
| ③ 只加深度 2（仍丢弃） | 31.0 | **50.0%** | 625 | 95.2% |
| ④ 深度 2 + 有界重试 | 34.3 | 100.0% | 0 | 97.0% |
| ⑤ 深度 3 + 有界重试 | 34.3 | 100.0% | 0 | 95.8% |
| ⑥ 深度 4 + 重试 8 次 | 34.3 | 100.0% | 0 | 94.7% |

⚠ 绝对数值未经真机标定（参数是量级估计），**有效的是相对趋势**。

## 发现一：当年"深度 2 → 30fps"很可能是**假象**

①→③ 对比：只加深度，**块送达率不变（都是 50%）**、主线程被卡也不变。
原因是深度只让模拟器"跑在前面"，**并不会让屏幕更快** —— 它改变的是
"同步调用多久返回"（也就是 fps 计数器读到的数），不是"屏幕上真正更新了多少内容"。
当年那条"15→30fps"的结论，很可能测的就是这个计数器。

⇒ **明早判断成败不能只看 fps 数字，要肉眼看屏幕**（是否有残留空洞、是否真的更流畅）。
这也说明：**这条路真正要降的是"每次推送的成本"，不是排队深度。**

## 发现二：丢帧会留下**永久空洞**（真 bug，已定位未修）

`rg_display.c:192-194`：

```c
if (screen_line_checksum[draw_top + y] != checksum)
{
    screen_line_checksum[draw_top + y] = checksum;   // ← 先记账"已画"
```

**先记录校验和、之后才交给驱动推**。而驱动那一步可能被 DMA2D 丢弃（实测很常见）——
于是这块内容被标记为"已画过"，**下一帧不会再推**，屏幕上就留下一个空洞，
直到那块画面内容自己变化（校验和变了）才会被补上。静态画面（比如暂停菜单）会一直花。

**修法**（本仓库已有先例，见同文件 552 行 `screen_line_checksum[top + y] = 0;`）：
推送失败时把对应行的校验和清零，强制下一帧重推。配合第 3 步的有界重试一起做最自然
（重试仍失败才算失败）。这条建议与 AXI-ICM 独立，**即使帧率没提升也应该修**。

---

# 夜间最终交付

**代码/工具**（均已 commit + push 到公开仓）：
- `mipi_dsi_tab5.h`：AXI-ICM QoS 提权、三段细分打点、DMA2D 忙时有界重试
- `rg_display.c`：队列深度参数加了完整注释（默认仍为安全的 1）
- `tools/sim_display_pipeline.py`：显示流水线仿真（新增）
- `tools/bench_transpose.c`：转置访存基准（新增）
- `docs/NIGHT-2026-09-25-DISPLAY.md`：本文（完整证据链与结论）

**镜像**（`dist/`）：
- `p2.6.4-ppa-revert.img` —— 已验证可跑的**回退基线**
- `p2.6.6-axi-icm.img` —— 打点 + QoS 提权（先刷这个）
- `p2.6.7-retry-qos.img` —— 再加有界重试
- `p2.6.8-depth2.img` —— 再加队列深度 2（实验档，有楔死史）

**尚未实施**：校验和空洞修复（发现二）、仿真参数的真机标定（依赖打点镜像的结果）。

---

# 真机实测（设备插回后，同日）

## 一、p2.6.6 游戏内数据（宝可梦弹珠台，稳定运行段每秒采样）

```
PERF: display=414.68ms/1000ms (transpose=414.46 submit=0.22) blocks=313
      [xpose=141.35 ovl=2.61 draw=269.60] rows=25.6 max=28
BUSY: 41%        FPS: 60 (30+30+0) / 40 (24+16+0)
underrun: 0 条   DMA2D 丢弃: 100 条/22 秒
```

| 指标 | 实测 | 结论 |
|---|---|---|
| `submit=` | 0.2ms/秒 | **提交开销可忽略** —— "DMA 提交/同步开销"假设排除 |
| `busy`/`ovl` | 2~4ms/秒 | 叠加层合成不是问题 |
| `xpose=` | 141ms/秒 | CPU 转置（读 PSRAM）≈ 每像素 24ns，比理论慢约 20 倍 |
| `draw=` | **270ms/秒** | 绝大部分是**在等 DMA2D**（非提交本身） |
| `BUSY` | 41% | **CPU 没跑满** → 不是算力瓶颈 |
| 有效吞吐 | ~40MB/s | 转置与 DMA2D 两条通路都被压在这个量级 |

**归因**：CPU 不满、提交不要钱、算力够用，但两条数据通路都只能到 ~40MB/s，
而面板 57.8Hz 扫描独自持续占用 ~107MB/s。⇒ **瓶颈是总线延迟**（视频流长突发占住总线），
不是任何一个环节的算力或算法。这与 IDF 在 DSI 欠载处的提示（用 AXI-ICM 优化内存带宽）指向同一处。

## 二、被否决的实验：降 DPI 时钟（→ 屏幕频闪）

依据上述归因，把 `vendor/m5stack_tab5/m5stack_tab5.c:1387` 的 `dpi_clock_freq_mhz`
从 70 降到 45（扫描 57.8Hz → 37.2Hz，理论释放 ~38MB/s）。

**结果：吞吐无改善，屏幕出现明显频闪，用户当场否决 → 已回退到 70MHz 并刷回 p2.6.6。**

**教训**：论证了"降刷新率不会欠载"，却没论证"降刷新率不会造成别的可见变化"。
刷新率是**用户看得见**的量，不能拿来换未验证的收益。
完整记录见 `~/esp32/ESP32-经验沉淀.md` §76。

## 三、下一个要动的旋钮（不改任何可见属性）

让面板**突发变短**而不是**变少** —— 扫描频率保持 57.8Hz（不频闪），但单次占用总线时间缩短，
给 CPU/DMA2D 留出空隙：

```c
/* 降低 DW-GDMA（IDF 的 DPI 扫描链路）突发令牌桶深度，N 从 32 试到 8 */
axi_icm_ll_set_qos_burstiness(AXI_ICM_MASTER_DW_GDMA_M0, N, AXI_ICM_ACCESS_READ);
```

判据：`draw=` 段耗时与 DMA2D 丢弃条数下降；`underrun` 仍为 0；**屏幕观感无变化**（这条是硬门槛）。
风险：把突发压得太小可能喂不饱面板 FIFO → 一有 underrun 就回退到更大的 N。


---

## 四、实验①：关掉 DMA2D（**被否决，已回退**）

做法：厂商 BSP `vendor/m5stack_tab5/m5stack_tab5.c` 的 `.flags.use_dma2d` 改 `false`，
改由 CPU 同步拷贝。分辨率/刷新率/色深/时序全未动。

真机结果（恶魔城）：

| 指标 | 关 DMA2D | 基线（DMA2D 开） |
|---|---|---|
| `previous draw operation is not finished` | **0** ✓ | 上百条/秒 |
| 横向割裂撕裂 | **出现** ✗ | 无 |
| BUSY | 64~79% ✗ | 41% |
| `draw=` | 165~282ms（不稳定）✗ | 270ms |
| `xpose=` | 73~124ms（不同游戏，不可比） | 141ms |

**否决理由**：撕裂属用户可见属性。
机制：DMA2D 路径的那个"抢不到就丢弃"的信号量，**本身就是我们唯一的 vsync 同步**
（它在刷新完成中断里释放）—— 关掉它，CPU 就直接往面板正在扫描的帧缓冲里写。
详见 `~/esp32/ESP32-经验沉淀.md` §78。

## 五、屏幕帧率计数器（**保留**）

需求来自用户："做一个显示 fps 的数字，在 L 和 R 按键中间的位置，这样我就可以明显看到每次变化了。"

实现：
- 位置：逻辑坐标 (640,60)，L 与 R 肩键之间（两键分别占 x 40~240 / 1040~1240，中间原本是空的）。
- 取值：`statistics.partialFPS + fullFPS` = **真正显示出去的帧率**，
  与日志 `FPS:(跳过+部分+完整)` 的后两项同口径。
- 通路：`rg_system.c` 的 `update_statistics()` 每秒调 `rg_overlay_set_fps()` 推给叠加层；
  `rg_touch_overlay.c` 用本模块已有的 8×8 点阵**逐像素直绘**（不走按键掩码 —— 数字每秒都变，
  重建掩码没意义），面积 3 位 × scale4 = 96×32 px，白字黑投影。
- 遵守既有约束：在显示驱动推给面板前的**最后一步**合成（`rg_overlay_blit_cw90`），
  与虚拟按键共用同一个合成点。

**真机验证**：恶魔城下屏幕显示 30，日志同期算出 21~29 → **两者一致** ✓（口径正确）。
注意：菜单等静态画面上会显示 1，这是诚实值（画面本来就没刷新），不是 bug。


---

## 六、E0 / E0b：内存带宽实测（**推翻了两个猜测**）

方法：驱动里加一次性探针，同一段代码在两个时刻各跑一次 ——
A 在面板开始扫描之前，B 在面板扫描中。缓冲 8MB（必须远大于 cache，否则测的是 cache）。

### E0 · 顺序访问（32 位）

| 条件 | 顺序写 | 顺序读 |
|---|---|---|
| A · 面板未扫描 | 83.1 MB/s | 89.0 MB/s |
| B · 面板扫描中 | 56.6 MB/s | 71.1 MB/s |

→ 面板扫描实测固定吃掉 **20~32% 的带宽**（以前只是猜测）。

### E0b · 真实访问模式（转置那种跨行模式）

| 访问模式 | A · 面板未扫描 | B · 面板扫描中 |
|---|---|---|
| **跨行读 64B**（每 1440B 碰一条 line） | **28.2 MB/s** | **28.0 MB/s** |
| **跨行写 2B**（旧转置的写） | **2.7 MB/s** | **1.8 MB/s** |

### 结论（三条，其中两条是纠正）

1. **我们的通路 29.7MB/s ≈ 跨行读上限 28.2MB/s —— 几乎相同。**
   所以转置**不是"没写好"，而是已经跑在它那种访问模式的天花板上**。
   ⚠ 更正：本文档早前写过"只用了三分之一内存能力"，那是对比错了对象
   （顺序 89MB/s 不是我们这种模式能拿到的），已作废。
2. **"面板抢带宽导致转置慢"被推翻**：跨行模式在扫描前后几乎不变（28.2 → 28.0）。
   面板吃的是顺序带宽，不是我们这条路的瓶颈。
3. **墙的物理本质**：28MB/s 有效字节 = 实际搬运约 **635MB/s 的 cache line**，
   利用率 1/22 —— 内存忙得冒烟，95% 的搬运是白搬的。

### E3 · 由 E0b 直接推出的改动（已实现）

既然问题是"碰得太少"，解法就是**每次多碰一点**：
源块的各行在内存里本来就紧邻（每行 1440B），所以**整块顺序读进片内 SRAM**
（48KB 暂存），再从 SRAM 转置 —— 坏模式从此打在 SRAM 上，没有 PSRAM 行缓冲惩罚。
块超过 48KB 时自动退回原分块路径，行为不变。
判据：`xpose=` 段耗时（基线 141ms/秒）与屏幕上的帧率数字。





---

## 七、E3 真机结果 + PPA 第二次重测 + 帧率数字的坑

### E3（整块顺序读进 SRAM 再转置）
- 恶魔城实测：**30 → 33 帧**（+10%）。是真的，但**有限** —— 它只打了"转置的读"那一趟，
  而"先拷进 SRAM"本身也有成本，净收益被吃掉一部分。
- 同一版上后来看到峰值 **39 帧**（不同场景，不能与 33 直接比；相对最初的 30，方向明确）。
- 启动健康：0 欠载 / 0 面板报错；HEAP 少 48KB（暂存缓冲如期分配）。

### PPA 第二次重测：**失败，且病根不是参数**（结案）
- 依据 R8T5（github.com/Layer812/R8T5）补上 `.data_burst_length = PPA_DATA_BURST_LENGTH_128`
  （我们原先完全没设）+ `max_pending_trans_num: 2 → 1`。
- 结果：**仍然 1 帧** → burst 不是病根（我先前的判断错了，记录在案）。
- 真正差别是**调用方式**：R8T5 **一帧一次** PPA；我们**一块一次** ——
  每秒几十次**阻塞** op，每次目的地都是跨行零碎写。
- 结论：**PPA 要用就整帧一次用，绝不能按块用。** 已回退 + 硬禁用，注释写清原因。
- **仍未验证**：整帧一次 PPA 到底多快 —— 这是唯一还没测过的形态。
  测法：一次 op 转整屏、打印耗时（可在"面板扫描中"再测一次拿真实数字）。

### 帧率数字的坑（两轮才修对）
1. 第一轮：以为"置脏行号算错" → 置脏逻辑行 44..75（用 cw90 映射验证过，**行号其实是对的**）。
2. 第二轮：以为 `update_statistics` 在游戏里不跑 → 日志 grep 推翻
   （游戏内每秒都有统计行，**值一直是新的**）。
3. 真因：**值没问题，断的是"画出去"那一段** ——
   - 数字只在"被推送的块正好覆盖它"时才被重画，游戏中脏区极少覆盖顶部正中；
   - 且 `tab5_fb`（帧缓冲指针）**只在 PPA 分支里赋值**，PPA 关着时是 NULL，叠加层无处可写。
4. 修法：① 无条件缓存 `tab5_fb`；② 在驱动的每秒 PERF 钩子（日志证明游戏内确实每秒执行）
   直接合成叠加层进帧缓冲，只对数字所在行带做 cache 写回（115200 字节，128 对齐），
   **不做整帧 1.84MB 写回**（那是上次"蓝屏不断闪烁"的最大嫌疑）；③ 直写前**先擦除**区域
   （不擦会新旧数字重叠 —— 真机已验证）。
- 现状：数字每秒自动刷新 ✓、无重叠 ✓、0 欠载 / 0 面板报错 ✓。
