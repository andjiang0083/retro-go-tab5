# PPA 记录校正（2026-10-08）：不重开，且两条旧说法要改

> 起因：读外部复刻仓 `losoco/retro-go-majula-pulic@st7701`（有一份跑得通的整帧 PPA）后，
> 一度判定"我们这边整帧形态从未真正跑起来过、值得重开"。**这个判定是错的** —— 竖屏线早在 9 天前
> 就把整帧形态量过并判死了。本文把两条旧说法的口径改对，并留下可查的出处。

## 一、结论

**PPA 不重开。** 本板（Tab5）已经量过三种形态，最近一次是**真实生产形态**：

| 形态 | 实测 | CPU 对照 | 出处 |
|---|---|---|---|
| 按块 + BLOCKING + 写 DPI 帧缓冲（横屏线，2026-09-25） | ~25ms/块，画面 1~2fps | 0.6~0.9ms/块 | `docs/archive/NIGHT-2026-09-25-DISPLAY.md` 第七节 |
| 240x160 →3x→ 720x480 纯缩放（竖屏线探针，2026-09-28） | 探针改造完成、buffer_size 补上 | — | 提交 `07b9be6` / `bed2bb1` |
| **A 整帧 720x480 一次 / B 按块 x15（竖屏线，2026-09-29）** | **A 16.8ms/帧（82MB/s）· B 1129µs/块** | 整帧 ~7.5ms · 566µs/块 | **`~/esp32/ESP32-经验沉淀.md` §125** / 提交 `578511d` |

机制解释（同一份 §125）：PPA 与 CPU 走的是**同一片 PSRAM 的同一档带宽**（82 vs 92~104MB/s），
而 `PPA_TRANS_MODE_BLOCKING` 下 CPU 还要白等硬件搬完 ⇒ 省下的转写时间被等待加倍还回去。
**显示路径定案：CPU 直写。**

## 二、要改的两条旧说法

### 1. "六轮真机全部 `err=0x102` ⇒ 还有别的原因，差异必在配置之外"（横屏线 2026-09-25）

**直接原因是探针漏给 `.out.buffer_size`，与参数/对齐/硬件无关。**

- 探针里 `ppa_srm_oper_config_t op = {0};` → `out.buffer_size = 0`；
- IDF（本机 5.5.2）`components/esp_driver_ppa/src/ppa_srm.c:210`：
  `ESP_RETURN_ON_FALSE(out_pic_len <= config->out.buffer_size, ESP_ERR_INVALID_ARG, ...)`，
  其中 `out_pic_len = 720 * 1280 * 2 = 1,843,200`（`ppa_srm.c:208`）⇒ `1,843,200 <= 0` 为假 → `0x102`；
- 这条校验**排在所有 scale / rotation 校验之前**（`ppa_srm.c:204` 之前只有 128 对齐、色域、in-block 范围），
  所以三个 scale 变体必然**同码死在同一行** —— 与"三变体均 0x102"的现象完全吻合；
- 这也解释了 `tools/patch-idf-dpi-fb-align.py`（帧缓冲 64→128 对齐）修完**毫无变化**：卡点不在对齐
  （那次修复本身是对的，保留）。
- 旁证：生产按块 PPA 分支一直写 `scale_x/y = 1.0f` 并且**真的跑起来过**（只是慢 25 倍）
  ⇒ 旧注释里"给 1.0/1.0 会被拒"是**误记**（已从横屏驱动里更正；该修正另存为补丁待应用到 `exp/display` 线）。
- 逐条对着 IDF 源码验算：`buffer_size` 给对之后，整帧 + 旋转 270° + `scale 1.0/1.0` 能通过**全部**校验
  （`rotation != 0/180` 时 `new_block_w = scale_y * in.block_h = 720 ≤ out.pic_w`、
  `new_block_h = scale_x * in.block_w = 1280 ≤ out.pic_h`，两项都是"刚好相等"）。

> 对上游（https://github.com/Layer812/R8T5/issues/1）若要回帖：答案就是 `out.buffer_size`；
> 顺便可以更正"R8T5 用 LovyanGFX 自管对齐缓冲"这条推测 —— 关键项是 `buffer_size`，不是缓冲来源。

### 2. "整帧一次这条形态至今从未真正跑起来过"

**只对横屏线 2026-09-25 那份记录成立**；竖屏线 2026-09-28/29 已经把它跑起来并量了（上表第三、四行）。
跨分支时，同一份 doc 的"已结案"可能是**两条线各自的、时间不同的**结论 —— 见 `ESP32-经验沉淀.md` §224。

## 三、外部参考仓（majula）今天还剩下什么价值

只有一条，而且**与 PPA 无关**：它用了 `esp_lcd_dpi_panel_get_frame_buffer(panel, 2)` 的**双 DFB**，
再由 `esp_lcd_panel_draw_bitmap` 呈现（写"没在被扫描"的那一块）。
我们 2026-09-25 单独把 `num_fbs` 从 1 改成 2 会**直接破坏绘制**（22 条面板报错 / 0 帧送达，已回退）。

但注意**动机**：换缓冲的意义是"不要写正在被扫描的缓冲"。我们的定案是 **CPU 直写**（同一块缓冲、
精确范围 msync），已经在这条路上跑到了 60fps 满帧。若将来因为**撕裂 / 校验和空洞**才需要换缓冲，
再去看它那份实现 —— 不要因为"它能跑"就引入双缓冲。

它的成本口径也记住：为了让 PPA 只写 viewport 的黑框干净，它**每帧对整屏输出缓冲
memset + msync(M2C)**（480×640×2 = 600KB/帧）；换到 Tab5 是 1.84MB/帧 ≈ 110MB/s —— 这笔账任何
"每帧一次 PPA"的方案都得先算。

## 四、本次实际改了什么（代码）

| 文件 | 改动 | 状态 |
|---|---|---|
| `components/retro-go/rg_display.c` | `rg_display_clear_rect` 补负坐标/越界裁剪（与 `rg_display_write_rect` 同款；先加 margins 再裁，保证 `rg_display_clear` 的 `-margins` 语义不变） | 已落地，编译通过 |
| `components/retro-go/drivers/display/mipi_dsi_tab5_p.h` | **未改**（竖屏驱动的 PPA 注释与探针已是校正后的版本，含 `buffer_size` 与真实用例） | — |
| `components/retro-go/drivers/display/mipi_dsi_tab5.h` | 横屏驱动的 0x102 口径校正（含可用的 A/B 探针） | **已退回**（本分支隔离规则）；补丁存 `/tmp/ppa-probe-landscape.patch`，待应用到 `exp/display` 线 |
