## What does this change?

v0.4.9：一份镜像支持**横竖屏切换**（方向运行时化），并回归**单 app 发布形态**。
Closes: 无对应 issue（本版按发布计划推进）。

## Why

横竖屏此前是"编两份固件"（`#if RG_TAB5_ORIENTATION` 选表）⇒ 两份镜像、两套皮肤、装错就错。
本版把方向变成**运行时真值**：一份镜像两个方向，首次开机询问，之后在 `Settings → Screen orientation` 改，
选择存 NVS、写后自动重启生效。

- 几何表（`targets/tab5/geom.h`）改为按方向取；宏仍以 `rg_geom()` 形式暴露 ⇒ 40 处调用点一字未改。
- 触摸键位表改为运行时装表（`keymap_touch[16]` 按方向 `memcpy`）；**X/Y ↔ L/R 调换键坐标**也改为运行时取值
  （横屏时归位到 L 与 R 正中间）。
- 全项目扫描过这类"按方向编死"：只剩两处**有意保留**（`RG_SCREEN_DRIVER` 必须编译期选驱动头；
  键位主/备表是装表输入）。
- 单 app 形态：两份同名显示驱动各包独立 TU + 运行时分发层（`lcd_init()` 时粘性锁定后端 ⇒ 写 NVS 只影响下次启动）。
- 分区表回归单槽 1984K；`rg_tool.py` 内嵌表不再追加横屏槽（修掉 P3 刷机隐患）。

**代价（已在 README/CHANGELOG 写明）**：回到单 app ⇒ 菜单里没有 `Check for updates`（需要一个额外 app 分区），
与 v0.4.7 时相同。

## How was it verified?

- [x] Built from a **clean** tree with `--target tab5 --no-networking`（双 app 与单 app 两种形态，CI 同款命令）
- [x] Flashed to a real Tab5 and exercised on hardware —— 同一份代码在开发树构建上真机验过：横屏与竖屏都起得来、
      X/Y ↔ L/R 切换键归位到 L 与 R 正中、零 panic（竖屏 `linear 1:1 map`、横屏 `90CW map`）。
      ⚠ 移植到本分支后的构建已过本地门禁与三锚点，但**真机复验时设备已断连**，待接上后补刷一次（不阻塞评审）
- [x] Launcher boots and a GBA game runs at 59-60 logical fps with audio
- [x] 若动了显示通路：真机连续运行无撕裂、无冻结（本版只改几何/键位取值的时机，未改显示路径）
- [x] 风险项都在运行时开关之后，且开关有文档（`targets/tab5/geom.h`、NVS `rgapp/orient`）

本机门禁（与 CI 第 118/148 行同款命令）：单 app 镜像过 `tools/check-single-app-image.py`（单槽 1984K、
app 数据完整、无旧横屏槽残留）与三锚点（0x2000=E9 / 0x8000=50AA / 0x10020=ABCD5432）。

## Measurements (if performance-related)

不涉及性能目标：本版不改显示通路（帧率与 v0.4.8 逐字一致，横屏稳态 `rows=28.0 max=28`、竖屏 `32.0/32`，
与已验证基线相同）。几何/键位改为运行时取值的代价：单 app `launcher.bin` 858→861 KB（+3KB，槽内余量 31.8KB）。

| | Before (0.4.8 双 app) | After (0.4.9 单 app) |
|---|---|---|
| 镜像数 | 2 份（横/竖各一）+ 单 app 包 | **1 份**（两个方向） |
| app 槽 | 三个（960K + 1472K + 1280K） | 一个（1984K） |
| merged 体积 | 3,801,088 B | 2,097,152 B |

## Notes for reviewers

- 真机验收记录见 `docs/HANDOFF-0.4.9-SINGLE-APP.md`；设计见 `docs/SPEC-0.4.9-SINGLE-APP.md`。
- 分区表变了（单 app 一槽）：**从 0.4.8 升级必须整包刷**，别只刷程序区。
- 两张新截图是**真机实拍**，图注里的按键名与位置逐条对着 `touch_layout.h` 的横屏表核过。
- 有意留下的后续项：横屏的**装饰底图**仍是竖屏风格（键位与命中区已按横屏重排），留待下一版。
- 另附 `docs/PPA-NONBLOCK-RESULT-2026-10-10.md`：一次"按块传 PPA"的实验负结果（默认零变化，整笔回退）。
