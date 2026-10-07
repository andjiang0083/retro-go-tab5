# GBA 内核（gbsp）与来源镜像的差距评估

**日期**：2026-10-07（同日修正一次，见 §二）
**状态**：⏸ **已分析固化，暂不推进**（等假期结束 → M5Burner 审核通过 → 更多用户接触 v0.4.7 之后再评估）
**触发缘由**：客户反馈《宝可梦弹珠台》"不支持 GBA ROM"
**结论一句话**：我们与来源镜像 `Irak4t0n/HowBoyAdvance` 的差距是 **20 个文件的零星差异**（不是"落后两代"），
**我们已经是新版渲染器**（含上游 2023-08 的 OBJ 透明修复）；值得摘的只有几处后期修复与功能增量。

---

## 一、⚠ 一次被推翻的假设（方法论留档，别重犯）

| 阶段 | 判断 | 依据 | 结果 |
|---|---|---|---|
| 初判（当天早些时候） | "我们的渲染器停在 2023-08 之前，落后上游两代，所以弹珠台 OBJ 透明 bug 还在" | `grep -ril 'mosaic\|semi_trans\|layer_order\|force_blend' --include=*.c --include=*.h` ⇒ **全 0 命中** | ❌ **被推翻** |
| 复判（全量 diff 之后） | 我们**已是新版**渲染器 | 同样的关键词在 **`video.cpp`（`.cpp` 不在上条 include 里）** 命中：`mosaic` 56、`layer_order` 10、`force_blend` 4、`backdrop` 13 —— **与对方完全一致** | ✅ |

**错因**：`--include` 少列了 `*.cpp`（上游 2023 年后把 `.cpp` 改名为 `.cc`，我们的树里仍是 `.cpp`）⇒ 假阴性。
**规矩**：判"某实现在不在我们树里"时，扩展名必须全列（`.c .cpp .cc .h .S`）；
单侧遗漏 include 造成的"我们什么都没有"，看起来和"真的没有"一模一样。

---

## 二、差距量化（2026-10-07 实测，`difflib` 逐文件）

两边都有的同名文件中，**只有 20 个有差异**，其余完全相同（`cpu.h` / `gba_cc_lut.*` / `memmap.h` /
`retro_inline.h` / `riscv/riscv_codegen.h` / `riscv/riscv_stub.S` / `video.h`）。

| 文件 | 我们 | HowBoyAdvance | 差异行 | 性质 |
|---|---|---|---|---|
| `gba_over.h` | 2194 | 1818 | **784** | 我们**更全**（游戏数据库条目更多）—— 同步前必须逐条核对 |
| `gba_memory.c` | 2678 | 3025 | **693** | 对方含大块功能增量 + 文件抽象改动（见 §四） |
| `input.c` | 243 | 20 | 241 | 我们的实现（触摸/按键），对方走自己的 app 层 |
| `cpu_threaded.c` | 3469 | 3586 | 235 | 主要是 `esp_cache` 的写法/注释差异（同一件事两种写法） |
| `serial.c` | 168 | 248 | 92 | 对方另有 `serial_proto.c`（RFU 联机） |
| `common.h` | 207 | 213 | 86 | 小差异 |
| `memmap.c` | 195 | 141 | 68 | 我们的 JIT 可执行映射实现 |
| `gbp.c` | 55 | 101 | 48 | — |
| `sound.c` | 833 | 867 | 46 | — |
| `riscv/riscv_emit.h` | 2234 | 2207 | 39 | **我们的独有资产**（周期记账口径修复） |
| 其余 10 个文件 | — | — | ≤32 | 小差异 |

对应关系（同源、扩展名不同，非"缺失"）：

| 我们 | 对方 | 差异行 | 相同块占比 |
|---|---|---|---|
| `video.cpp` (2339) | `video.cc` (2400) | **99** | **97.9%** |
| `cpu.cpp` (3636) | `cpu.cc` (3638) | **70** | **99.0%** |

**对方独有**：`gpsp_esp.c/.h`(301) + `gpsp_main.c`(455)（它的平台/应用层，对应我们的 `main.c`）、
`libretro.h`、`serial_proto.c`(724)、`bios_data.S`、以及 `arm/ mips/ x86/`（我们不需要）。
**我们独有**：`main.c`、`video.cpp`/`cpu.cpp`（= 对方的 `.cc`）。

上游主仓 `libretro/gpsp` **没有 `riscv/`**（只有 `arm/ mips/ x86/`）⇒ **同步对象是 HowBoyAdvance，不是主仓**。

---

## 三、`video.cpp` 到底差了什么（99 行全部看过）

1. **一处真 bug 修复（值得摘）**：把 mosaic 的 `if (!mosaic || !(i % mosh))` 改成计数器形式 `mctr`。
   对方注释原文：
   > *"Before this fix the loop counter 'i' was declared but never incremented; gcc constant-folded
   > 'i % mosh' to !0 and the mosaic skip never triggered, so **the rotated-bitmap mode-3/4/5 paths
   > sampled every pixel instead of every Nth**."*

   ⇒ 我们的版本里 `i` 没自增 ⇒ 常量折叠 ⇒ **mode 3/4/5 旋转位图的 mosaic 效果完全失效**；
   顺带计数器形式省掉每像素一次 `divl`。
2. **性能小优化**：每条扫描线把 `io_registers` 的 layer priority 读 16 次预取成 4 次（`layer_prio[4]`）。
3. 其余为注释与写法差异。

---

## 四、`gba_memory.c` 到底差了什么（对方多 520 行）

对方独有行关键词分布：`flash` 30 / `EEPROM` 11 / `SRAM` 9 ⇒ **存档类型检测与 flash 处理大改**。
具体新增结构（对方）：

- `gamepak_file_blocks`（物理载荷按 32KB 块计）、`gamepak_mirror_1m`（**1 MiB Classic NES / Famicom Mini 镜像模式**）、
  `gamepak_mini_rom` + `gamepak_mini_materialized`（mini ROM 物化）、`gamepak_header_nonstandard`
- `RFILE *gamepak_file_large`（改用 libretro 的文件抽象）、`is_known_game` / `require_m1_hle_bios`
- flash/EEPROM/SRAM 检测逻辑更完整（含 `Don't let a scanned EEPROM signature override real SRAM/flash access` 这类修复）

我们独有：`ESP_PLATFORM` 下的 `heap_caps` PSRAM 分配、`EXT_RAM_ATTR`、Macronix flash 常量、
以及我们自己的探针（`RG_GBA_INPUT_TRACE`）。

> ⚠ **冲突点**：对方的 `gamepak_file_blocks` / mirror / mini-ROM 物化模型，与我们的
> **分块映射 + 缺页回读**（`gamepak_buffers` + `evict_gamepak_page()` + `fseek` 随机回读）不是一回事。
> ⇒ `gba_memory.c` **不能整文件替换**，只能按 hunk 摘。

---

## 五、若将来要同步：怎么动（不整仓、不整文件）

| 优先级 | 摘什么 | 为什么 | 风险 |
|---|---|---|---|
| P1 | `video.cpp` 的 mosaic 计数器修复（+ 顺带 layer_prio 预取） | **真 bug**（mode 3/4/5 mosaic 失效），改动局部、可独立验 | 低（画面对比：打开/关闭 mosaic 的游戏） |
| P2 | `gba_memory.c` 的 flash/EEPROM/SRAM 检测增补 | 存档兼容类收益（可能解释若干"存档报错/丢档"反馈） | 中（要保留我们的分块/缺页模型） |
| P3 | 1 MiB mirror（Famicom Mini 系列）、nonstandard header | 新游戏支持面 | 中 |
| P4 | `serial_proto.c`（RFU 联机） | 新能力，非缺陷 | 中（要接 `rfu.c`/`serial.c`） |
| — | `bios_data.S`、`gpsp_esp.c/.h`、`gpsp_main.c`、`libretro.h` | 是对方的平台/应用层，**我们不需要** | — |

**必须保留的我们独有资产**：v0.4.4 dynarec 周期记账口径对齐（`riscv/riscv_emit.h`，39 行差；
HowBoy 与上游的 `ws_cyc_nseq/ws_cyc_seq` 命中数均为 0）、`gba_over.h`（我们更全）、
retro-go 接口层（`main.c` app 黏合、按键含 L/R/X/Y+Turbo、`memmap.c` JIT 映射、PSRAM 分配、
显示通路 3× 缩放/转置/`rg_display_push_failed` 丢块重标脏、中文 UI 与触摸手柄）。

**验证判据（每摘一片）**：宿主（解释器）逐帧 PNG 先过 → 真机回归 = 现有 6 个 ROM 无退化 +
帧率不低于 59.7fps + 存档能读回 → 出一版可刷回镜像，旧版保留。

---

## 六、客户问题的现状（仍未解决）

「渲染器落后两代」已被推翻 ⇒ **《宝可梦弹珠台》的根因目前是未定的**。已知边界：

- ROM 侧健康（title `POKEPIN R/S`、code `BPPJ`、8 MiB、header 校验和正确、`.srm` = 131072 B = Flash 128K）。
- 我们**已有**上游 2023-08 的 OBJ 透明修复与重启修复（因为渲染器已是新版）⇒ 上游 issue #210 的两个症状
  **理论上都不该出现**。
- 仍可能的方向：① 我们的 **RISC-V dynarec** 特有分歧（本项目有前科：周期记账口径差 1 帧导致按键窗口错位）；
  ② 我们的**显示通路**（历史记录里"卡在启动画面"就是它，launcher 也能复现）；③ `gba_memory.c` 缺的
  flash/EEPROM/SRAM 检测增量（若客户症状是存档相关）；④ 客户固件版本较旧（未含 v0.4.4/v0.4.5 的修复）。
- **先要三件事**：具体现象（花屏 / 重启 / 冻住 / 报错）、固件版本、复现率。
  零真机成本的第一刀：宿主（解释器）跑同一 ROM 逐帧 PNG；真机第二刀：dynarec vs 解释器 A/B。

---

## 七、相关挂起项（同一条"承诺/实现不一致"的线）

- **GBA zip 声明**：`launcher/main/applications.c` GBA 条目写了 `"gba zip"`，gbsp 无任何
  `rg_storage_unzip_file` 分支 ⇒ 列表能点、点了 `RG_PANIC` → `abort()` → 重启；
  `README.md` + `dist/m5burner-0.2…0.4.6` 共 **14 处**文案承诺了 zip。方案 A（撤声明 + 清文案 + 启动前门禁）**未实施**。
- **失败回执**：启动 GBA 前打一行 ROM 体检（title/code/size/存档类型/是否走缺页回读），panic 时把最后几行
  落 `sd/retro-go/logs/last-crash.txt` —— 让下一次"某游戏不支持"自带判据。

---

## 八、外部信息索引

- 上游 issue：**#210**（Pokemon Pinball R&S 图形+重启，2023-08-27 以 "new video code" 关闭）、
  **#230**（该游戏的 rumble 选项不生效）、#196（ARM32 dynarec 竖条，参考）
- 来源镜像：`Irak4t0n/HowBoyAdvance`（ESP32-P4 + 自写 RISC-V dynarec，README："Uses gpSP with a custom RISC-V dynamic recompiler"）
- 上游主仓：`libretro/gpsp`（无 `riscv/`；2023-08 合并 `cxx_impl` 的 "new video code"）
- 本机方法论：`tab5-device-telemetry-debug` 技能 → `references/gba-game-compat-forensics.md`
