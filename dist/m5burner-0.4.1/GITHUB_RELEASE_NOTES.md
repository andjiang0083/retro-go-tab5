# retro-go Tab5 v0.4.1 —— 竖屏版 · 充电修复 / Portrait layout, charging fixed

M5Stack Tab5（ESP32-P4）上的 **GBA 模拟器**。竖屏布局：游戏画面（720x480，GBA 240x160 的 3x 整数缩放）
固定在原生竖屏面板顶部，触摸手柄在**下方控制区**，按键更大、不遮挡画面。

A port of [retro-go](https://github.com/ducalex/retro-go) to the M5Stack Tab5, reworked for the panel's
**native portrait orientation**: the game screen is pinned to the top, the touch gamepad moved to the
control area below it.

## 这一版修了什么 / What's fixed in 0.4.1

**插 USB-C 不充电** —— 板级充电使能 `CHG_EN` 从未被拉高：BSP 的 IO 扩展器初始化里注释写着
"P7(CHG_EN) 输出高电平"，实际赋的值却只开了 P0/P3，于是充电芯片 IP2326 一直处于禁用状态，
插着线也不进电（关掉充电指示灯也永远看不到）。已照官方 demo 的初始化序列显式补上使能并真机验证：
充电电流 **-0.75 ~ -0.87 A**，电池电压持续上升（7627 → 7745 mV）。

**USB-C charging was never enabled** — the board's `CHG_EN` pin stayed low (the vendor BSP's IO-expander
init writes `0b00001001` even though its own comment claims P7/CHG_EN is driven high), so the IP2326
charge IC was disabled and the pack never charged. Fixed by mirroring the official demo's init sequence.

同版还修了发布图上的文字：PC 重渲染工具的字模解析口径已与固件对齐（此前图上 L/R/SELECT/START/MENU
全是左右镜像），封面中文字体不再静默回退成方块。电量圆灯的"充电中"从 1 Hz 硬闪改为**柔和呼吸**。

## 刷机 / Flashing

- **M5Burner**：选 `Tab5` 类别 → `retro-go Tab5` → 版本 **0.4.1** → **Burn**
- **esptool**（本附件是 merged 全镜像，从 0x0 整片写入）：
  ```
  esptool --chip esp32p4 write_flash -z 0x0 retro-go-tab5-0.4.1-merged.bin
  ```
- 校验附件：
  ```
  shasum -a 256 -c retro-go-tab5-0.4.1-merged.bin.sha256
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
| `retro-go-tab5-0.4.1-merged.bin` | M5Burner / esptool 用的 merged 全镜像（2,490,368 字节，DIO） |
| `retro-go-tab5-0.4.1-merged.bin.sha256` | 校验和（`648f76f1…`） |
| `cover-320x200.png` | M5Burner 封面（真渲染的竖屏界面 + 设备外框） |
| `README_M5Burner.md` | 逐字段刷机说明 + 按键表 |

布局校验 / layout check：`bootloader@0x2000`（`e903024f`，DIO + ESP32-P4）· `partitions@0x8000`（魔数 `aa50`）·
`launcher@0x10000` · `gbsp@0x100000`。**前 64 KB 与 v0.4 完全一致**（bootloader 与分区表都没动，差异全在 app 段）。
固件版本戳 `v0.0.1-44-g55691`（ESP-IDF v5.5.2）。
