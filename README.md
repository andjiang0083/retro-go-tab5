# retro-go Tab5 —— M5Stack Tab5 上的 GBA 模拟器（竖屏版）

M5Stack Tab5（ESP32-P4，1280x720 原生竖屏）上的 **Game Boy Advance 模拟器**，
基于 [retro-go](https://github.com/ducalex/retro-go) 移植，界面按面板的原生竖屏重做：
游戏画面（720x480 = GBA 240x160 的 3 倍整数缩放）贴顶占满宽度，触摸手柄在下方控制区。

A Game Boy Advance emulator for the **M5Stack Tab5** (ESP32-P4, 1280x720 native portrait),
ported from [retro-go](https://github.com/ducalex/retro-go). The UI is reworked for the panel's
portrait orientation: the game screen (720x480, integer 3x of the GBA's 240x160) is pinned to the
top, the touch gamepad sits in the control area below.

---

## 下载与安装 / Download & install

**先看你是从哪儿装 —— 本工程提供两份固件，装错的那份会"只有菜单、进不了游戏"。**

| 安装渠道 | 用哪个文件 | 为什么 |
|---|---|---|
| **M5Burner**（推荐）或 esptool | `retro-go-tab5-<版本>-merged.bin` | 完整镜像（bootloader@0x2000 + 分区表@0x8000 + launcher@0x10000 + gbsp@0x100000），从 0x0 整片写入。功能最全，菜单里保留"检查更新" |
| **[M5Launcher](https://github.com/bmorcelli/Launcher)** | `retro-go-tab5-<版本>-launcher-singleapp.bin` | 该平台**只能装单个 app 镜像**。完整镜像是两个 app（菜单 + 核心各占一个分区），从它装进来只会装上菜单、核心起不来。这份把两者编进同一个镜像 |

安装包与校验值见 [`dist/`](dist/)（每个版本一个目录，含 `README_M5Burner.md`、变更说明与封面图）。

> 单 app 包的差异：菜单里没有 "Check for updates"（它需要额外的 app 分区来落新固件），
> 升级请用 M5Burner / esptool 整包刷。其余功能完全一致。

**Install channel matters — there are two firmware builds.** M5Burner / esptool users take the *merged*
image; M5Launcher can only install a single app image, so it needs the *single-app* build (menu and
emulator core compiled into one image). Pick the wrong one and you get a menu whose games never start.

## 使用 / Usage

- **ROM**：GBA ROM 放 SD 卡的 `roms/gba/`（`.gba` 或 `.zip`，zip 内放一个 ROM）
- **存档**：`retro-go/saves/gba/`，游戏内存档跨重启持久化
- **封面**：可选，`romart/<ROM 文件名>.png`
- **操作**：竖屏布局 —— 方向键（左下）、A/B/X/Y 菱形（右下，X/Y = Turbo A/B 连发）、
  L/R 肩键（控制区顶部左右角）、**X/Y ↔ L/R 调换键**（顶部正中，点一下把菱形位与肩键位整对互换）、
  SELECT/START（底部中间）、MENU（游戏内菜单）、电量圆灯（控制区正中）
- 界面语言默认英文，可在 **Options → Language** 切换中文（设置保存）

详细说明见 [`dist/`](dist/) 里对应版本的 `README_M5Burner.md`。

## 从源码构建 / Building from source

```sh
. tools/idf-env.sh                       # ESP-IDF 5.5.x 环境
cd retro-go-p4

# 双 app 形态（默认，M5Burner / esptool 用）
python3 rg_tool.py --target tab5 --no-networking build-img launcher gbsp

# 单 app 形态（M5Launcher 用；菜单与核心编进同一个 app）
python3 rg_tool.py --target tab5 --no-networking --single-app build-img launcher
```

产物是 `retro-go_<版本>_tab5.img` / `..._tab5-single.img`（整片合并镜像）；
单 app 形态的纯 app 镜像在同一次构建的 `launcher/build/launcher.bin`。
刷机脚本见 `tools/flash-retro-go.sh`，抓日志见 `tools/log-tab5.sh`（复位）/ `tools/log-tab5-nr.sh`（不复位）。

> 两种形态由编译期开关 `-DRG_SINGLE_APP=1` 切换，源码只有一处分歧点
> （`components/retro-go/rg_system.c` 的 `update_boot_config()`）。
> 注意 **它同时是 CMake 缓存变量，必须显式传 0/1** —— `rg_tool.py` 已经这么做了。

移植文档、触摸手柄布局模板与踩坑记录见 [`docs/`](docs/)。

## 已知事项 / Known issues

- 开机瞬间串口上会有一批 `ST7123`（触摸一体屏）I2C 报错，属既有现象，不影响使用
- 没有蓝牙手柄时，触摸手柄是唯一输入源（叠加层强制常显）
- 崩溃时 SD 卡根目录会生成 `crash.log`，反馈问题请带上它

## 许可与致谢 / License & credits

- 本项目基于 [ducalex/retro-go](https://github.com/ducalex/retro-go)（GPLv2）移植；
  GBA 核心为 **[gpSP](https://github.com/libretro/gpsp)**（GPLv2），
  本移植的 RISC-V 动态重编译（dynarec）派生自 **[HowBoyAdvance](https://github.com/Irak4t0n/HowBoyAdvance)**（GPLv2）
  —— 因此整体以 **GPLv2** 发布，见各目录下的 `COPYING`
- 板级支持来自 M5Stack 的 Tab5 BSP（`vendor/m5stack_tab5`）
- Tab5 竖屏移植、触摸手柄、性能与安装兼容性工作：andjiang
