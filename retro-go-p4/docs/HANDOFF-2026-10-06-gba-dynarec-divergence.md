# 真因与修复：GBA《恶魔城 晓月之圆舞曲》在 dynarec 下按键失效（ESP32-P4 / retro-go-tab5）

日期：2026-10-06 · ROM `/tmp/aos.gba`（sha256 前缀 `a4ca7213…`，code=`A2CJ`，8 MB）
状态：**真因已定位到代码行，修复已落地并通过编译验证；真机 A/B 待设备接上后执行（脚本已就绪）**

---

## 1. 症状

| 构建 | 菜单光标变化次数 | PC 分岔 | 游戏自身帧计数 |
|---|---|---|---|
| 解释器（正确） | **21** | — | 基准 |
| dynarec（原状） | **12~13** | `f=285` 起永久分岔 | 自 `f≈293` 起**恒定少 1** |

按键**送达正常**：两边 `GBA_INPUT` 都是 162 条，dynarec 侧 `GBA_P1_READ` 读到的键值（`03F7`/`03FE`/`03FF`）逐位正确 ⇒ 问题在**游戏状态/时序**，不在输入。dynarec 并非「完全不响应」，而是「部分响应 + 相位错位」（序列与解释器同型，少走几步）。

## 2. 真因：SWI 指令的等待周期记账，两边取的区域不同

解释器对每条指令的周期扣减发生在 `cpu.cpp` 的 `skip_instruction`：

```c
skip_instruction:
    cycles_remaining -= ws_cyc_seq[(reg[REG_PC] >> 24) & 0xF][1];
```

**注意它用的是「指令执行**之后**」的 `reg[REG_PC]`。** 而 ARM SWI 分支（`cpu.cpp:3029-3040`）在跳转前就把 PC 改成了 BIOS 入口：

```c
case 0xF0 ... 0xFF:
    ...
    reg[REG_PC] = 0x00000008;                          // → BIOS SWI 入口
    reg[REG_CPSR] = (reg[REG_CPSR] & ~0x3F) | 0x13 | 0x80;
```

⇒ **解释器给 SWI 记的是 `ws_cyc_seq[0][1]` = BIOS 区 1 拍。**

dynarec 侧同样每条指令扣一次，但用的是「**该指令自己所在区**」：

```c
// cpu_threaded.c
#define arm_base_cycles()   cycle_count += def_seq_cycles[pc >> 24][1]
#define thumb_base_cycles() cycle_count += def_seq_cycles[pc >> 24][0]
```

⇒ **对从 ROM/EWRAM 发出的 SWI，dynarec 记 `def_seq_cycles[8][1]` / `[2][1]` = 6 拍。**

表值（`gba_memory.c`）：

```
def_seq_cycles / ws_cyc_seq:  BIOS {1,1}  EWRAM {3,6}  Gamepak(wait0,默认) {3,6}
```

⇒ **每个 SWI 差 5 拍，方向恒定。**

**为什么这会让游戏错位**：GBA 的 `VBlankIntrWait` 有著名硬件行为 —— 调用时若 VBlank 中断标志（`REG_IF` bit0）**仍挂着**就**立即返回**，不等下一帧。dynarec 多记周期 ⇒ 客机相对模拟时间**偏滞后** ⇒ 它执行 BIOS 的 `IntrWait` 去读 `REG_IF` 时，VBlank 事件**更可能已发生且尚未被应答** ⇒ 读到置位 ⇒ **立即返回 ⇒ 游戏主循环多转一圈** ⇒ 帧计数恒定少 1（帧锁定，故不继续漂移）⇒ 启动状态机错位 ⇒ 输入处理再也对不上帧 ⇒ 光标只响应一部分。

游戏每帧的 `VBlankIntrWait` 是 SWI、`f=285` 的首次 LZ77 解压也是 SWI（都由游戏代码发出）⇒ 相位偏差在这里越过分岔临界点。

## 3. 证据链

**决定性证据（三条独立）**
- **A**：`DIAG_DIR`（游戏自身状态字节 `0x0200001A`）—— `f=285..292` 两构建都因解压暂停 8 帧；恢复后解释器减 1 次、dynarec 减 2 次，此后恒定差 1。
- **B**：区域哈希（expG，每 10 帧采样 f=10…600）—— **EWRAM 256KB 全同**，**唯一不同是 IWRAM `0x03007000-0x03007FFF`（BIOS 中断/栈区）**，从第一个采样点起持续。
- **C**：帧边界中断标志 —— 解释器 `IF=0004`（仍 pending），dynarec `IF=0000`（已应答）⇒ 两者对 VBlank 的**相位**不同。

**已排除（全部受控实验）**

| # | 假设 | 实验 | 结论 |
|---|---|---|---|
| 1 | 输入层 | `GBA_INPUT`/`GBA_P1_READ` | ❌ 键值正确 |
| 2 | 显示层 | 推像素哈希/丢块 | ❌ drops=0 |
| 3 | 翻译缓存陈旧/SMC | 每帧强制 flush（f<600） | ❌ 分岔一字不差 |
| 4 | 编译优化 -O3 | 换上游 `-Oz -fno-exceptions -fno-rtti` | ❌ 分岔相同 |
| 5 | ROM 分页/映射 | `ROM_BUFFER_SIZE` 2→32（21MB 常驻，零换页） | ❌ 分岔相同 |
| 6 | `touch_gamepak_page` 缺失 | 比对两侧调用点 | ❌ 两边都没有 |
| 7 | `smc_write_arm_yes` 越界 | 比对 + 调用点 | ❌ 死代码 |
| 8 | RISC-V 后端 | `riscv/riscv_emit.h`、`riscv_stub.S` 逐字节 diff | ❌ 逐字节相同 |
| 9 | `arm_swi`/`thumb_swi` 桩本身 | 逐字节 diff | ❌ 相同（都不加周期） |
| 10 | 解释器 `BX` 少 `arm_pc_offset(8)` | 查分发 | ❌ dynarec 构建从不调 `execute_arm` |
| 11 | `update_gba` 被改 | 与上游 `gpsp_main.c` 全文 diff（181 行） | ❌ 逐字节相同 |
| 12 | `main.c` 被改 | 与上游全文 diff（455 行） | ❌ 逐字节相同 |
| 13 | VBlank 触发点 | `IRQ_VBLANK` 位置/策略 | ❌ 相同 |
| 14 | 周期预算掩码 | `cycles_to_run(c)=(c&0x7FFF)` vs dynarec `and reg_cycles,a0,0x7FFF` | ❌ 一致 |
| 15 | 块偏移位宽 | `hashhdr_type.blk_offset` | ❌ 完整 u32 |

⇒ 结论：**不是移植回归，也不是后端 bug，而是「翻译期周期记账口径」与解释器不一致** —— 上游 dynarec 同样如此（`riscv/` 逐字节相同），属**上游同样存在的精度缺口**，值得回报上游。

## 4. 修复

`gbsp/components/gbsp-libretro/cpu_threaded.c`，ARM/Thumb 两处 SWI 分支内补回差值（只针对**真正去 BIOS** 的 SWI；div SWI 6/7 走 HLE 不去 BIOS，不动）：

```c
// ARM（case 0xF0 ... 0xFF 的 else 分支）      行 1787
cycle_count += def_seq_cycles[0][1] - def_seq_cycles[pc >> 24][1];   // 6 → 1
arm_swi();

// Thumb（case 0xDF 的 else 分支）            行 2360
cycle_count += def_seq_cycles[0][0] - def_seq_cycles[pc >> 24][0];
thumb_swi();
```

算术自检：ROM(8)/EWRAM(2) 净 = 1 ✅；IWRAM(3) 净 = 1−1 = 0 ⇒ 不变 ✅（本就与 BIOS 同值）。

**编译验证**：`rg_tool.py --target tab5 --no-networking build gbsp` 成功；`compile_commands.json` 确认 `cpu_threaded.c` 带 `-DHAVE_DYNAREC -DHAVE_JIT -DRISCV_ARCH`；产物 `gbsp/build/gbsp.bin` = 1212096 B。

## 5. 验证计划（待设备接上）

脚本：**`~/.hermes/cache/scratch/expH_swifix.sh`**（一条命令：构建 → 刷机 → 185s 抓日志 → 对比解释器基线 `/tmp/ab_A_interp.log`）。
判据：
1. PC **全程无分岔**（尤其不再有 `f=285` 起的永久分岔）；
2. 光标变化次数 **≈21** 且序列同型（DOWN=+9 / UP=−9 / LEFT=−1 / RIGHT=+1）；
3. `DIAG_DIR` 不再恒定少 1。

## 6. 已知的同类残余差异（若验证不通过，按此顺序继续）

1. **Div SWI（6/7）**：dynarec 内联 HLE 只记 `cycle_count += 64`（注释自承 "Big under-estimation"），解释器则把 SWI 6/7 送进 BIOS 真跑除法（几百拍）⇒ 若游戏在关键路径用它，偏差更大。**须真机探针统计实际执行的 SWI 号**才能定论（裸 ROM 字节扫描不可靠：8 MB 里大都是压缩数据，2 字节模式必然误命中）。
2. **跨区直接分支**：解释器按分支**目标**区扣，dynarec 按分支指令自身区扣 ⇒ 同区（绝大多数）无差，跨区（进/出 BIOS）才有差。
3. **`def_seq_cycles` 是 `const` 静态表，`ws_cyc_seq` 会随 WAITCNT 写而更新**（`reload_timing_info()`）⇒ 若游戏改 WAITCNT，两者会持续不一致。需探针确认游戏是否写 `0x04000204`。

## 7. 工作区状态（实验残留，注意）

| 文件 | 现状 |
|---|---|
| `gbsp/components/gbsp-libretro/cpu_threaded.c` | **含本次修复（2 处）** |
| `gbsp/components/gbsp-libretro/CMakeLists.txt` | 编译选项为上游 `-Oz -fno-exceptions -fno-rtti`（expE 改）；`RG_GBSP_DYNAREC` 当前 **ON**；备份 `/tmp/CMakeLists.gbsp.bak` |
| `gbsp/components/gbsp-libretro/gpsp_config.h` | `ROM_BUFFER_SIZE` = 32（expF 改，等价上游） |
| `gbsp/components/gbsp-libretro/gba_memory.c` | ROM 缓冲改从 PSRAM 分配（expF） |
| `gbsp/main/main.c` | 探针 + `RG_TEST_FLUSH_CACHE` 钩子 + 区域哈希探针（`#ifdef` 隔离） |
| `components/retro-go/targets/tab5/config.h` | `RG_TEST_KEYS_DEVICE`（40 段）、`RG_TEST_NO_AUTOSAVE 1`、`RG_TEST_FLUSH_CACHE 0`、`RG_GBA_DIAG 1` |

真机回刷 backrooms：`write_flash @~/esp32/tab5-backrooms/flash_args.in`。

## 8. 产物

- 日志：`/tmp/ab_A_interp.log`（解释器基线）、`/tmp/ab_B_dyn.log`、`/tmp/ab_F.log`、`/tmp/ab_G_{interp,dyn}.log`、`/tmp/{expC,expE,expF,expG}.log`、`/tmp/build_expH.log`
- 脚本：`~/.hermes/cache/scratch/{ab_dynarec_input,expC_flush,expD_regions,expE_optflags,expF_rombuffer,expG_regions,expH_swifix}.sh`
- 上游参照：`/tmp/hba`（含 `DEVLOG.md`）


---

## 9. 结论（2026-10-06 收尾，**真机已验证**）

### 根因
两个引擎的**周期记账模型不一致**：解释器按"区域表 + 实际地址"动态扣费，dynarec 写死常数。
⇒ 同一段重活（LZ77 解压 / 关卡加载）在两引擎里跨的 VBlank 帧数差 1 ⇒ **游戏自身计时相位永久错 1 帧** ⇒ 输入/动画窗口整体错位。这也解释了为什么所有"时序补丁"都无效——它们都没动到这个口径。

### 修复（`riscv/riscv_emit.h` + `cpu_threaded.c`，解释器为权威）
| 口径 | 修复前（dynarec） | 修复后（= 解释器） |
|---|---|---|
| 访存 8/16 位 | 写死 load +2 / store +1 | 运行时查 `ws_cyc_nseq[区域][0]` |
| 访存 32 位 | 同上 | 运行时查 `ws_cyc_nseq[区域][1]` |
| 取指 | `def_seq_cycles[pc>>24][1]`（ROM=6） | `ws_cyc_nseq[pc>>24][0]`（ROM=5） |
| MUL/MLA/长乘/Thumb-MUL | +2/+3 估算（13 处） | 0（解释器不扣） |

运行时表值（默认 WAITCNT，`reload_timing_info`）：`ws_cyc_nseq[8]={5,9}`、`ws_cyc_seq[8]={3,6}`、`ws_cyc_nseq[2]={3,6}`。

### 真机验证（同一 185s 抓取窗口、脚本按键时间轴固定）
| 构建 | 模拟帧率 | 光标事件 | 光标值序列 |
|---|---|---|---|
| 旧 dynarec | 37.5 | 13（全挤在 2603 帧后） | 1,2,4,5,7,8,9 ❌ |
| 修复①（仅访存） | 59.3 | 29 | 9,24,39,0,9,24,9 |
| **修复②（+取指+MUL）** | **59.7** | 29 | **9,24,39,0,9,24,9** ✅ |
| 解释器（基准） | 34.7 | 21 | 9,24,39,0,39,24,9 ✅ |

用户真机确认：**帧率正常 + 字母输入界面按键全部正常**。
性能反直觉但真实：扣费更真实 ⇒ 每帧要执行的 guest 指令更少 ⇒ 吞吐 **37.5 → 59.7 帧/秒**（解释器 34.7）。

### 残余差异（已定性，暂不修）
- 与解释器差**恒定 1 帧**（f≈9~50 一次相位翻转），**速率完全相同**（f=50→400 两者计数器均下降 342）⇒ 不是漂移。
- 性质：**结构性**。dynarec 在**块边界**结算周期/中断，解释器按**每条指令**结算 ⇒ 帧边界落点可差一个块（帧边界 PC：dynarec `0x0B30` 块首 vs 解释器 `0x0B34` 块内）⇒ 游戏内一次二选一判定翻转。
- 影响：无功能影响（输入正确、速率一致、60fps）。

### 后续可选（未做；会**增加** dynarec 扣费，改动前先备份并复测）
1. LDM/STM 每寄存器 `+1` → 解释器的 `ws_cyc_seq[区域][1]`（EWRAM=6）
2. 直接分支 B/BL/BX 未扣 → 解释器的 `ws_cyc_nseq[目标区域][1]`

### 测试脚手架（全部关闭，未删除）
`components/retro-go/targets/tab5/config.h`：`RG_GBA_DIAG 0`、`RG_GBA_INPUT_TRACE 0`、`RG_TEST_KEYS_DEVICE ""`、`RG_TEST_NO_AUTOSAVE 0`、`#undef RG_TOUCH_TRACE`。
⚠️ 两个坑：`RG_TOUCH_TRACE` / `RG_TEST_KEYS_DEVICE` 是 **`#ifdef` 判定**，设 0 无效（必须不定义 / 置空字符串）；测试脚手架必须与功能修复**分开**关闭，否则用户"用不了设备"。
发行版日志：`/tmp/ab_release2.log`（脚本按键/输入追踪/按键读取/存活探针/诊断/触摸日志 全 0）。固件：`gbsp/build/gbsp.bin` = 1223888 B。
