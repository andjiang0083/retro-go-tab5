# 外部仓库精读：losoco/retro-go-majula-pulic（st7701 分支）

- 扫描日期：2026-10-08
- 对象：https://github.com/losoco/retro-go-majula-pulic （默认分支 = `st7701`）
- 取证方式：`git clone --depth 1 --single-branch --branch st7701`（157MB）+ GitHub API（谱系/提交）+ 与我们 `retro-go-p4/` 逐文件 `diff -u | grep -c`
- 目的：判断它对我们的 Tab5 移植有没有可用增量

---

## 一、结论（一句话）

**核心层零增量，显示平台层有一份"目标可达"的对照样本。**

它是 `longxiangam/retro-go` 的复刻 fork（Majula 掌机：ESP32-P4 + 2.8" 640×480 MIPI **1-lane** ST7701，IDF 5.3.1）。
它的自有工作量几乎全在显示与板级 BSP；**它有一份跑通的「整帧 PPA SRM 直接写 DPI 帧缓冲 + 双 FB + draw_bitmap 呈现」实现** ——
恰好是我们这边判过死刑的两件事（整帧 PPA 探针三变体全 `0x102`；`num_fbs=2` 单独改导致绘制全失败）。
所以它的价值在「复核已判死路径时的对照物」与「若干小修」，**不是可整段照抄的代码**。

---

## 二、谱系与分量（硬证据）

| 项 | 值 | 证据 |
|---|---|---|
| 谱系 | fork of `longxiangam/retro-go`，default_branch `st7701` | GitHub API `/repos` 的 `fork`/`parent.full_name` |
| 自有提交 | 2026-08-11 ~ 08-17，约 20 条（作者 luozuochen） | API `/commits?sha=st7701` |
| 前置历史 | longxiangam 的 P4 移植线（2025-08 ~ 2025-11） | 同上，更早提交为上游/移植史 |
| 面板 | 640×480 (480×640 portrait) MIPI DSI **1-lane** | `targets/majula/`, `docs/2.8TR+ST7701S MIPI 1Lane .txt` |
| IDF | **5.3.1** | `targets/majula/sdkconfig: CONFIG_IDF_INIT_VERSION="5.3.1"` |
| 许可 | GPL-2.0（与我们 `COPYING` 一致） | 双方 COPYING 逐行相同 |
| GBA 核心 | **无 RISC-V JIT**：`gbsp/components/gbsp-libretro/` 只有 `arm/ mips/ x86/ 3ds/` | 目录列表；`find -type d -name 'riscv*'` 为空 |

⇒ **性能层没有可摘的东西**：他们的 GBA 天花板是解释器；我们的 `riscv/` dynarec 才是 Tab5 满帧的来源。
他们也没有任何帧率仪表（`grep -riE 'perf|fps'` 只命中上游残留），所以它 README/提交里任何性能说法都不构成证据。

对比（我方 `retro-go-p4` vs 对方树，`components/retro-go/` 同名文件差异行数）：

```
871 rg_input.c   （我们有整套触摸 overlay/skin，他们没有）
795 rg_gui.c
715 rg_display.c （我们的差异主要在"只提交变化行"与两条渲染路径）
454 rg_system.c
422 translations.h（我们的 ZH 词条）
  0 rg_localization.c / rg_settings.c / rg_audio.h  ← 上游共享代码，不是他的贡献
```

---

## 三、可摘清单（按优先级）

### P0 — 整帧 PPA SRM + DFB 双缓冲的对照实现（**已判：不重开，仅存档参考**）

> ⚠ **2026-10-08 复核后作废**：本节当初的判定（"值得重开整帧 PPA 实验"）是**错的**。
> 竖屏线早在 2026-09-29 就把整帧形态量过并判死（整帧 16.8ms/帧 vs CPU 7.5ms，慢一倍；
> 出处 `~/esp32/ESP32-经验沉淀.md` §125）。口径校正与 0x102 真因见
> [`PPA-RECORD-CORRECTION-2026-10-08.md`](PPA-RECORD-CORRECTION-2026-10-08.md)。
> 本节保留，仅作为"当初为什么会误判"的证据与它的实现笔记。
> 今日余下的唯一价值：它的**双 DFB + `draw_bitmap` 呈现**形态（与 PPA 无关），
> 等我们因撕裂/校验和空洞需要换缓冲时再回来看。

**他们的形态**（`components/retro-go/drivers/display/st7701.h`）：

| 要素 | 他们的做法 | 位置 |
|---|---|---|
| 输出缓冲 | `esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fbs[0], &fbs[1])` ⇒ 双 FB | `st7701.h:302` |
| 换帧 | `cur_fb_index = (cur_fb_index+1) % 2` | `st7701.h:541` |
| 缩放/旋转 | **每帧一次** `ppa_do_scale_rotate_mirror`，整帧不是按块；`rotation 90`；`mode = PPA_TRANS_MODE_BLOCKING` | `s_srm_ops_fit/full` |
| 参数显式度 | `in/out` 都给 `pic_w/pic_h/block_w/block_h`；FIT 把 viewport 位置放进 `out.block_offset_x/y` | 同上 |
| 呈现 | PPA 写完再 `esp_lcd_panel_draw_bitmap(panel, left, top, w, h, buffer)` | `lcd_send_buffer_ppa` |
| 同步 | DPI 传输完成回调给信号量 + `lcd_sync()` 阻塞等 | `lcd_color_trans_done_cb` / `lcd_sync` |
| 模式分流 | OFF/FIT/ZOOM 走 PPA，FULL 由核心 CPU 预缩放 | `rg_display.c:294-359` |

**与我们 0x102 那次失败的可查差异（复核时的「这次哪里不同」清单）**：

1. `.out.buffer_size` **显式给出**（我们的整帧探针没设这项）。
2. `out.pic_w/pic_h` 用**旋转后**尺寸（他们 `RG_SCREEN_HEIGHT/WIDTH`），不是面板物理 W/H。
3. `scale_x/scale_y` 传**真实比例** `min(viewport/src)`，不是 `1.0/1.0`（我们注释里已发现 `1.0/1.0` 会被拒）。
4. 位置走 `out.block_offset_x/y`（旋转后 x/y 互换：`left = viewport_top`、`top = viewport_left`）。
5. **PPA 写的是"当前没在被 DPI 扫描"的那一块 DFB**，再由 `draw_bitmap` 呈现；
   我们当年是直写"正在被 DPI 扫描"的帧缓冲 —— 这正是我们在 `TAB5-PORT-STATUS.md` 里诊断的带宽争抢根因的**规避方式**。
6. IDF 5.3.1 vs 我们 5.5.2（PPA 驱动版本差异，不能默认行为一致）。

**成本警告（先量再做）**：他们 FIT 路径**每帧对整屏输出缓冲** `memset` + `esp_cache_msync(M2C)`
（黑框必须纯黑，否则残影闪烁）。他们那边 480×640×2 = 600KB/帧 ≈ 37MB/s @60fps；
换到 Tab5 是 1280×720×2 = **1.84MB/帧 ≈ 110MB/s** —— 直接照抄很可能把带宽吃光。
我们已有「只提交变化行」（exp/display 的 P2-2），两者必须合起来设计。

**判据（沿用我们自己的）**：显示占比 <20% 就别做；验收 = 同走位对比 `tools/parse-display-baseline.py`
的负载态数字，且逐像素一致。**不预告收益倍数。**

### P1 — 「进出游戏爆音」的功放时序（`drivers/audio/i2s.c`，+28/−9）

他们的修法：
- 静音：**先关功放**，再 `i2s_zero_dma_buffer`
- 取消静音：先清 DMA → `rg_task_delay(20)` → 再开功放
- `driver_deinit()`：先 `set_mute(true)` + 20ms 再 `i2s_driver_uninstall`
- 功放使能抽成 `set_amp_enabled(bool)`，初始化时先 `false`

对照我们：Tab5 走 ES8388（`drivers/audio/tab5_es8388.c` 只有 `esp_codec_dev_set_out_mute`），
旧形态 `i2s.c` 里仍是一次性 `zero_dma` + 直接置脚。
⇒ 若真机在 启动器 ↔ 游戏 切换时听到"噗"，按这个顺序改；没有 pop 就不动。

### P1 — `rg_display_clear_rect` 负坐标/越界裁剪（+17 行）

他们先裁剪 `left/top < 0` 与超出 `real_width/real_height`，再进 PPA 路径（否则负坐标 LCD window 警告）。
我们 `rg_display_clear_rect` 不做裁剪，直接把 `left/top`（含 margins）送 `lcd_set_window`。
GBA/SNES 清黑框时可能出现负坐标 ⇒ **3~5 行的防御，值得补**。

### P2 — SNES：`frameskip 0` + 手写最近邻缩放（不推荐照抄）

他们：`app->frameskip` 由 `3` 改 `0`，并手写 640×480 最近邻
（x：`a,a,b,b,b`；y：2 行重复、每 7 行补 1 次）；非 FULL 模式直接提交原始分辨率。
我们：SNES `frameskip = 3` + 原始帧直接提交，缩放走显示通路（`rg_display`）。
两种设计取向不同，且**他们没有仪表 ⇒ "SNES 满帧"不能当结论**。
唯一可做的是**在我们这边量 SNES 的显示占比**：若大头在显示通路，P0 那次实验才是解药；若在核，就与显示无关。

### P2 — 状态栏电池百分比（+8 行，`rg_gui.c`）

在电池图标右侧画 `%d%%`。我们只在 debug 菜单里有 `%.2f%% | %.2fV`。小功能，可选。

### P3 — 编译期默认值：`RG_LANG_DEFAULT=ZH` / `RG_FONT_DEFAULT=SansLight20` / `RG_TIMEZONE_DEFAULT=CST-8`

他们在 `components/retro-go/config.h` 里改默认。**对照我们**：
`rg_localization.c` 静态初值 `RG_LANG_EN`，`rg_gui.c:117` 从 settings 取默认 `0`（=EN）
⇒ **首次开机是英文**。若产品面向中文用户，这是一行默认值的事（值不值得改由你定）。
注意他们的 `SansLight20` 是 **7.2MB 的 C 数组字体**，我们不需要（见下条）。

### P3 — 字体方案：我们是更优解，只借鉴一点

| | 他们 | 我们 |
|---|---|---|
| 中文 | `FusionPixel12.c` 4.0MB + `Sans16.c` 5.2MB + `SansLight20.c` 7.2MB，全编译进 app | `assets/cjk12.bin` **105KB** / 3773 字形，`target_add_binary_data` + mmap |
| 代价 | app 体积暴涨（十几 MB） | flash 独立段，app 不涨 |

⇒ 唯一可借鉴：**遇到缺字就扩 `tools/gen_cjk_font.py` 的字符集**（成本在 flash，不动 app）。

---

## 四、他们已经修、但我们早已有（不必再看）

- GBA L/R 肩键映射 —— 我们 `gbsp/main/main.c:116` 已有（他们 08-12 才补）。
- SFC X/Y/L/R 六键映射 —— 我们 `retro-core/main/main_snes.c:29-31` 已有。
- GBA 即时存档 —— 我们已实现（`GBA_STATE_MEM_SIZE` 416KB + 校验），他们这份是补课。
- GBA FIT/ZOOM 生效性 —— 我们的 ZOOM 是**整数倍 + 宽度/控制区双向钳制 + 贴顶**（`rg_display.c:485-520`），
  比他们事后补的「超屏时等比钳制」更稳。
- 本地化框架 —— `rg_localization.c` 两边**逐行 0 差异**（上游共享），不是他们的贡献。
- 触摸 —— 他们 targets 无触摸；我们整套 touch overlay/skin 无交集。

---

## 五、待办（具体、可执行）

> ⚠ **2026-10-08 复核后改写**：原第 1 条（重开整帧 PPA 实验）**已作废** —— 见
> [`PPA-RECORD-CORRECTION-2026-10-08.md`](PPA-RECORD-CORRECTION-2026-10-08.md) 与 `§125`。下面只留仍成立的三条。

1. **音频 pop 复核**：刷现有固件，专注听 启动器 ↔ 游戏 切换；有 pop 才按 P1 改 `tab5_es8388.c` / `i2s.c`。
   （用户 2026-10-08 决定：此项**暂缓**。）
2. **`rg_display_clear_rect` 裁剪防御**（3~5 行，低风险）—— **已落地**（`rg_display.c`，编译通过）。
3. **首次开机默认语言**确认为英文即可（`rg_localization.c` 初值 + `rg_gui.c:117`）—— 用户 2026-10-08 决定**不改**。
