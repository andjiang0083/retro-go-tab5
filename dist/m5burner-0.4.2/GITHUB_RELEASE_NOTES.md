# retro-go Tab5 v0.4.2 —— 竖屏版 · 肩键可换位 / Portrait layout, swappable shoulder keys

M5Stack Tab5（ESP32-P4）上的 **GBA 模拟器**。竖屏布局：游戏画面（720x480，GBA 240x160 的 3x 整数缩放）
固定在原生竖屏面板顶部，触摸手柄在**下方控制区**，按键更大、不遮挡画面。

A port of [retro-go](https://github.com/ducalex/retro-go) to the M5Stack Tab5, reworked for the panel's
**native portrait orientation**: the game screen is pinned to the top, the touch gamepad moved to the
control area below it.

## 这一版新增 / What's new in 0.4.2

**X/Y ↔ L/R 调换键** —— L 与 R 之间多了一颗琥珀色按钮。点一下：菱形位上那对换成 R / L，
两个肩键位换成 X / Y。格斗、赛车这类"肩键和菱形位都得用"的游戏，可以把肩键挪到更好点的地方。
按钮标签常显"菱形上现在是哪一对"，状态断电保存（NVS）。

**Swappable shoulders.** A new amber button between L and R swaps the two pairs: the diamond gets
R / L, the shoulder positions get X / Y. The label always shows which pair currently sits on the
diamond, and the choice survives a reboot.

**修：虚拟按键在游戏里的按压反馈** —— 此前只有切界面才会刷新（按下去不亮、调换键点了不变，
要进出游戏才生效）。根因是显示层推帧**只覆盖游戏视口**（720×480），而触摸手柄那条控制条带
在视口外的黑边上，推帧循环根本到不了那些行。现在由显示层对"外观变了的按键矩形"单独重画
（背景取边框图或纯黑），按压高亮与新标签都当场出现。

**Fixed: on-screen button feedback inside games.** It used to refresh only when you switched
screens — pressing a button gave no highlight, and the swap key appeared to do nothing until you
entered or left a game. The display layer's frame-push loop only covers the game viewport
(720×480), while the touch pad lives in the black margin outside it, so those rows were never
redrawn. The display layer now repaints exactly the rectangles whose appearance changed.

**修：调换键在默认 X/Y 模式下功能是反的** —— 按 X 发出的其实是 R、按 Y 发出的其实是 L
（亮度也跟着错位：按 X 亮起的是肩键 R）。输入层漏了"按当前状态决定映射"这一步，
自反对换被无条件应用，等于永远停在已调换那一侧。现在输入层与可视层共用同一个映射函数，
结构上不可能再不一致。**这条会让"调换键点了没反应/功能不对"的观感彻底消失。**

**Fixed: the swap key was functionally inverted in the default X/Y mode** — pressing X actually
emitted R and pressing Y emitted L (the highlight lit the shoulder instead). The input layer
skipped the state check, so the self-inverse swap was applied unconditionally, leaving the mapping
permanently on the "swapped" side. Input and display now share one mapping function.

同时修掉两处相关缺陷 / Two related fixes:

- **列表里点触摸键偶尔短暂花屏** —— 之前在输入线程里直接要求"整屏重画"，等于两个线程抢同一块
  界面缓冲。现在所有重画都回到显示线程。
  *Brief screen corruption when tapping in the launcher* — a cross-thread full redraw used to be
  requested from the input task, two threads drawing the same UI surface. All repainting now
  happens on the display thread only.
- **手指抬起后按键不释放** —— 那段"抬指复位"写在了"本帧有触点"的分支里，无触点的回合一整拍
  跑不到；表现为调换键只能切一次、按键偶发粘住。
  *Keys could stick after lifting the finger* — the release-reset sat inside a branch that only
  runs when a touch point is present, so it was skipped on the frames without one.

**方向键 8 向判定改用矢量扇区**（十字外观一个字没改）：斜向更准，不再串到相邻方向。

**D-pad diagonal accuracy** — the cross looks identical; the hit test is now sector-based, so
diagonals are reliable and no longer bleed into the neighbouring direction.

## 刷机 / Flashing

- **M5Burner**：选 `Tab5` 类别 → `retro-go Tab5` → 版本 **0.4.2** → **Burn**
- **esptool**（本附件是 merged 全镜像，从 0x0 整片写入）：
  ```
  esptool --chip esp32p4 write_flash -z 0x0 retro-go-tab5-0.4.2-merged.bin
  ```
- 校验附件：
  ```
  shasum -a 256 -c retro-go-tab5-0.4.2-merged.bin.sha256
  ```

## SD 卡 / SD card

```
<SD 卡根目录>/
├── roms/gba/            ← GBA ROM（.gba 或 .zip，zip 内放一个 ROM）
├── retro-go/saves/      ← 游戏内存档（跨重启保留）
├── romart/              ← 可选封面图（游戏.gba → 游戏.png）
└── crash.log            ← 崩溃日志，反馈问题时请带上
```

## 电量圆灯 / Battery light

控制区正中那盏圆灯：绿 ≥60% ／ 橙 20~60% ／ 红 10~20% ／ <10% 红闪 ／ **充电中绿呼吸**。

## 附件 / Assets

| 文件 | 说明 |
|---|---|
| `retro-go-tab5-0.4.2-merged.bin` | M5Burner / esptool 用的 merged 全镜像（2,490,368 字节，DIO） |
| `retro-go-tab5-0.4.2-merged.bin.sha256` | 校验和（`89a9898c…`） |
| `cover-320x200.png` | M5Burner 封面（真渲染的竖屏界面 + 设备外框，含新的调换键） |
| `README_M5Burner.md` | 逐字段刷机说明 + 按键表 |
| `screen-hero.png` | 可选附件：触摸手柄真渲染全图（720×1280，发布说明里可作配图） |
| `feature-swap.png` | 可选附件：调换键两个状态对照图（菱形位 X/Y ↔ L/R） |

布局校验 / layout check：`bootloader@0x2000`（`e903024f`，DIO + ESP32-P4）· `partitions@0x8000`（魔数 `aa50`）·
`launcher@0x10000` · `gbsp@0x100000`。**前 64 KB 与 v0.4.1 完全一致**（bootloader 与分区表都没动，差异全在 app 段）。
固件版本戳 `v0.0.1-69-g08046`（ESP-IDF v5.5.2）。

包含 v0.4.1 全部内容（含"插 USB-C 不充电"修复与圆灯呼吸）。
