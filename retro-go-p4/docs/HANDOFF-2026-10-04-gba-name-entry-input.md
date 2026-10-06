# 交接：GBA 名字输入界面方向键错乱（2026-10-04 凌晨）

## 症状
《恶魔城·晓月之圆舞曲》名字输入/存档选择界面：按「左」光标从 A(1行1列) 跳到 K(2行2列)（视觉"下+右"）；
按「上」回到 A。"游戏内一切正常，只有这类静态界面错"。ROM 已在安卓模拟器验证正常。

## 根因（已定位，机制明确）
`gbsp/components/gbsp-libretro/input.c:154` 原为：
    write_ioreg(REG_P1, (~old_key) & 0x3FF);
GBA `KEYINPUT(0x04000130)` 的 bit10~15 在真机上为"释放=1"（浮空），原代码写成 0 ⇒ 整字读取
KEYINPUT 的界面会看到 6 个幽灵"按下"位。游戏内移动只取低 10 位掩码，故"只有菜单错"。
读取路径（`gba_memory.c` 的 `readaddress`/dynarec 快路径）都直接返回 `io_registers[REG_P1]`，不补位。

## 已做的修复（已刷入真机）
    write_ioreg(REG_P1, ((~old_key) & 0x3FF) | 0xFC00);
- 真机构建件：`gbsp/build/gbsp.bin`（2026-10-04 00:13），`esptool write_flash 0x100000`，写后校验通过。
- **尚未经用户端症状验证**（等真机按三下确认）。

## 已排除
触摸坐标/矢量命中/按键映射（真机遥测 8/8 正确）；dynarec（关掉走解释器仍错）；
ROM（安卓模拟器正常）；KEYCNT 中断（该界面零写）；显示丢块计数（drops=0）。

## 宿主复现通道的坑（三次失败教训）
`build-sdl2/gbsp` + `RG_TEST_KEYS` + `RG_TEST_DUMP` 可无人值守复现，但要走到名字输入界面：
1. 只按 START ✗ → 卡在存档选择界面之前；
2. 只按 START→A→方向 ✗（40s 后不再按 START）→ **掉进 attract/demo 循环出不来**；
3. 正确做法：**全程持续按 START**（打断 demo）+ A（选存档进名字界面）+ 方向键交替，并用 dump 帧
   的"静态段"判据确认到位（名字界面：帧间只有光标小簇，几百~2千像素；整屏数万像素=还在动画/demo）。
运行限时：macOS 无 `timeout`，用 `perl -e 'alarm shift; exec @ARGV' <秒> <命令>`。
分析：需要 numpy+PIL（本机默认 python3 无）→ `/tmp/venv_np/bin/python`，脚本 `/tmp/an.py`、`/tmp/an2.py`。

## 待办（按优先级）
1. **用户真机验证**：存档选择 → A → 名字界面 → 左右上下各按一下，光标是否规矩。
2. 若症状仍在 → 显示层"每帧强制全刷"版（旁路 `screen_line_checksum`，对比强刷前后），
   判据：强刷后光标立刻正常 ⇒ 显示层"只推变化行"背锅。
3. **虚拟手柄高亮慢一拍**（用户实测：第一次按不亮、第二次才亮，功能正常）：
   机制已定位 —— `rg_touch_overlay.c:848` 在合成时读 `rg_input_get_pressed_mask()`，
   而输入在另一个线程更新 ⇒ 合成读到上一次采样。修法是同步/顺序，需谨慎动线程边界。
4. 宿主逐格核对（脚本按上面第 3 条修正后）。
5. 收尾：关掉 `RG_TOUCH_TRACE`（`targets/tab5/config.h`）、清理 `RG_TEST_KEYS_DEVICE` 残留。
