# 已解决：GBA 菜单/名字输入不响应 —— 根因是 RISC-V dynarec

（2026-10-06，真机 ESP32-P4 / Tab5，retro-go 0.4.3 + gbsp(gpSP)）

## 症状
游戏内 KEYINPUT 读到的值**完全正确**，但菜单/光标不响应（上游 issue #3
"Equip menu ignores Down"）。用户目标：恶魔城《晓月之圆舞曲》名字输入界面能移动光标、输入字母。

## 根因（已用真机 A/B 闭合）
**RISC-V dynarec（JIT）把游戏状态算错了**，与输入层、显示层都无关。

判据：同一 ROM、同一套按键时间轴、同一套显示几何下，比对**游戏帧缓冲哈希**
（`rg_hash(gba_screen_pixels, 240*160*2)`，只反映游戏自身逻辑结果，与显示/输入采样无关）：

| 帧号 | 宿主（解释器） | 真机（dynarec） | 真机（关 dynarec） |
|---|---|---|---|
| 1 → 241 | `F4EB239B` … `00352972` | 一致 | 一致 |
| **301（≈5.0s）** | `782756F2` | **`233E459B` ✗** | **`782756F2` ✓** |
| 361/421/481/541 | — | 继续分叉 ✗ | 逐帧一致 ✓ |
| 601+ | — | ✗ | 只在按键时间轴开始后因时序偏移分叉 |

第一段按键在 t=8s（f≈480）⇒ **f=301 的分歧与输入无关** ⇒ 两边唯一差别（dynarec）即元凶。

旁证：
- 显示层清白：游戏期 `drops=0`、无 `draw failed`、推送计数正常；ROM 选择界面的
  `dpi_panel_draw_bitmap: previous draw operation is not finished` 是**背压**（重试成功）。
- 输入层清白：issue 自带日志即证明。
- 关掉 dynarec 后真机与宿主**逐帧一致**（f=1..541 全同）⇒ 因果闭合。

## 修复
`gbsp/components/gbsp-libretro/`：

1. `CMakeLists.txt`：新增 `option(RG_GBSP_DYNAREC ... OFF)`（默认走解释器）。
   关 JIT 时 `COMPONENT_SRCDIRS` 不含 `riscv/`（那是 dynarec 的寄存器桩），
   编译选项不含 `-DHAVE_DYNAREC -DHAVE_JIT -DRISCV_ARCH`。
   ⚠ 这三个宏不能写成 CMake 关键字塞进 `rg_setup_compile_options()` 的参数列表（会被当编译选项）。
2. `gpsp_memory_alloc.c`：那批 PSRAM 全局定义用 `#ifdef HAVE_DYNAREC` 包住 ——
   解释器路径由 `cpu.cpp` 的 `#ifndef HAVE_DYNAREC` 块定义同一批全局（`EXT_RAM_ATTR`，同样落 PSRAM），
   两边都定义必然 `multiple definition`（这是上游"关 dynarec"这条路径没走通的原因）。

代价：晓月实测 ≈40fps（P4 可接受）；dynarec 版虽 60fps 但状态是错的。
要开 JIT：`idf.py -DRG_GBSP_DYNAREC=ON`，**但必须先修 dynarec 的 bug**。

## 复现 / 验证方法（可复用）
1. 宿主（含真机显示几何）：`build-sdl2/gbsp-diag`，环境变量 `RG_TAB5_DISPLAY_EMU=1 RG_GBA_DIAG=1`
   + `RG_TEST_KEYS` 按键时间轴；真机侧用编译期 `RG_TEST_KEYS_DEVICE`（真机没有环境变量）。
2. 每 60 帧打一条 `DIAG_FB f=<帧号> game=<游戏帧哈希> push=<累计推送像素哈希> n=<推送次数> drops=<丢块>`。
3. 按**帧号**（不是墙钟）对齐宿主与真机，第一处分歧即 bug 位置。
4. A/B 只改一个变量（dynarec 开/关），同 ROM、同输入、同几何。

## 真机操作坑
- `write_flash` 末尾的 "Hard resetting via RTS pin" 会把 P4 留在 **download 模式**；
  刷完必须补 `esptool.py --after watchdog_reset read_mac`。
- 抓运行中日志用**不复位**变体（`tools/log-tab5-nr.sh`），否则端口跳变把设备打进下载模式。
- 无人值守要先解决"进游戏"：启动器需要两次输入 ⇒ 给 launcher 加 `RG_TEST_BOOTROM`
  （`rg_system_switch_app("gbsp","gba",<rom>)`）或 gbsp 侧 `RG_TEST_AUTOROM`。

## 状态
- 真机已刷：官方 0.4.3 launcher + 干净的解释器版 gbsp（无时间轴/强制 ROM/探针），
  自检 `emulation loop=1`、钩子痕迹 0、非 download 模式。
- 待办：①把 dynarec 的 bug 整理成上游 issue（后端移植自 `Irak4t0n/HowBoyAdvance`，GPLv2）；
  ②真机肉眼确认名字输入界面光标可动（哈希级证据已闭合，但面板无摄像头）。
- 回 backrooms：`esptool.py write_flash @~/esp32/tab5-backrooms/build/flash_args.in`
