# retro-go Tab5 —— M5Burner 刷机包 v0.3

M5Stack Tab5 上的 **GBA 模拟器**（retro-go 移植）。本包为 M5Burner 专用 **merged 全镜像**
（bootloader@0x2000 + 分区表@0x8000 + launcher@0x10000 + gbsp@0x100000，整片从 0x0 写入）。

## 刷机

1. M5Burner 里选 **Tab5** 类别，找到 **retro-go Tab5**
2. 选版本 **0.3** → **Burn**（或用本目录的 `retro-go-tab5-0.3-merged.bin` 走 Custom FW 导入）
3. 刷完自动重启；首次插卡开机会自动创建 `roms/gba/` 等目录

## SD 卡目录结构

```
<SD 卡根目录>/
├── roms/gba/            ← GBA ROM 放这里（.gba 或 .zip，zip 内放一个 ROM）
│   └── gba_bios.bin     ← 可选：GBA BIOS
├── retro-go/            ← 模拟器工作目录（自动创建，一般不用手动碰）
│   ├── saves/           ← 游戏存档
│   ├── config/          ← 设置、最近游玩、收藏
│   ├── cache/ themes/ borders/
├── romart/              ← 游戏封面图（可选，按 ROM 文件名匹配：`游戏.gba` → `游戏.png`）
└── crash.log            ← 崩溃时自动生成；反馈问题请带上这个文件
```

## 操作方式

Tab5 几乎没有物理按键，手柄画在触摸屏的画面留白区：

| 按键 | 位置 | 说明 |
|---|---|---|
| 方向键 | 左侧留白区 | |
| A / B / X / Y | 右侧留白区，菱形排布 | X / Y = 核心的 **Turbo A / Turbo B**（按住连发） |
| L / R | 顶部左右两侧 | GBA 肩键 |
| START / SELECT | 底部中间 | |
| MENU | 右下 | 打开游戏内菜单（存档 / 选项 / 重置） |
| OPTION | — | 选项菜单 |

界面语言默认英文，可在 **Options → Language** 切换为中文（设置会保存）。

## v0.3 更新内容

- **显示性能大幅提升** —— 显示通路重做：整块顺序读进片内 SRAM、总线 QoS 提权、像素转置改为 32×32 分块。
  游戏帧率从原先的 ~15-20fps 提升到 **稳定 30fps**（GBA 类游戏）。
- **稳定性** —— 显示通路在忙时改为**有界重试**（原先会静默丢帧，表现为局部画面停止更新）。
- 包含 v0.2 的全部内容（中文支持 / 崩溃与按键修复）。

---

# retro-go Tab5 — M5Burner package v0.3 (English)

A **Game Boy Advance emulator** for the M5Stack Tab5 (retro-go port). This is a **merged flash image**
for M5Burner (bootloader@0x2000, partition table@0x8000, launcher@0x10000, gbsp@0x100000, written from 0x0).

## Flashing

1. In M5Burner pick the **Tab5** category and find **retro-go Tab5**
2. Choose version **0.3** → **Burn** (or import `retro-go-tab5-0.3-merged.bin` via Custom FW)

## Controls

The Tab5 has almost no physical buttons, so the gamepad is drawn on the touch screen in the letterbox margins:
D-pad (left) · A/B/X/Y diamond (right, X/Y = the core's **Turbo A / Turbo B**) · **L/R** (top corners) ·
START/SELECT (bottom centre) · MENU (in-game menu) · OPTION (options).

## What's new in v0.3

- **Much faster display path** — rebuilt: block-sequential reads into on-chip SRAM, bus QoS boost,
  and the pixel transpose reworked into 32x32 tiles. Games went from ~15-20 fps to a **stable 30 fps**.
- **Stability** — the display path now retries with a bound when busy instead of silently dropping frames
  (which used to leave parts of the screen frozen).
- Includes everything from v0.2 (Chinese support / crash and button fixes).
