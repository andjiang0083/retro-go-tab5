> **这是一份开发日志，不是当前状态说明。**
> 当前功能状态请看 [README.md](../README.md)，待办请看 [ROADMAP.md](../ROADMAP.md)。
> 日志里较早的计划（例如"用 PPA 硬件 SRM 替掉 CPU 转置"）后来被实测否定了 —— 保留原文是为了让后来者知道**试过什么、为什么不行**。
> 下面的"已实测确认的硬件事实"那张表请当作硬约束：**不要重新试错**。

# Tab5 retro-go 移植状态（滚动更新）

> 项目根：`~/esp32/retro-go-tab5/`
> 分支基线：`rapha-tech/retro-go` @ `ESP32-P4-clean`（自带 esp32-p4 target + gbsp）
> 环境：IDF v5.5.2（`~/esp/esp-idf-v5.5`）+ `~/.espressif/python_env/idf5.5_py3.14_env`
> 构建/刷机一律走 `tools/idf-env.sh`、`tools/flash-retro-go.sh`、`tools/log-tab5.sh`

## 一、已实测确认的硬件事实（不要重新试错）

| 项 | 结论 | 证据 |
|---|---|---|
| 面板 | ST7123 一体屏，**原生竖屏 720×1280** | M2a 真机点亮；BSP `bsp_display_get_panel_ic()` 返回 ST7123 |
| 横向 DPI 流 | **不可用**：把 DPI 切成 1280×720 + 补发 MADCTL 0x23 → 背光亮但全黑 | M2b 实测（日志 `alive frames=300 draw_errs=0`，驱动层无错、面板拒收） |
| 官方做法 | 竖屏原生时序 + 上层旋转 | M5GFX `panel_width=720/height=1280`；BSP `sw_rotate=true`；官方 demo `LV_DISPLAY_ROTATION_90` |
| 逻辑↔物理映射 | **顺时针 90°**：逻辑(lx,ly) → 物理(719−ly, lx) | M2c 四角锚点图案真机目视确认（tag v0.2），用户独立描述四角颜色吻合 |
| 触摸 IC | ST7123（一体屏），I2C 地址 0x55，INT=GPIO23，坐标系为面板物理 720×1280 | BSP `bsp_display_indev_init_to_st7123`（含本地补丁注释） |
| 触摸地址坑 | BSP 只对外暴露 GT911 版 `bsp_touch_new()`（0x5D）→ 本屏不能用，必须自建 ST7123 panel io | `bsp/touch.h` vs `m5stack_tab5.c` 实现 |

## 二、显示驱动架构（`drivers/display/mipi_dsi_tab5.h`）

- 面板初始化全交给官方 Tab5 BSP（`bsp_display_new_with_handles_to_st7123`），**绝不手写 `esp_lcd_new_panel_dpi` + DBI init**（P4 rev1.3 会挂 task WDT，7/13 卡死根因）
- retro-go 侧逻辑分辨率 1280×720（`RG_SCREEN_WIDTH/HEIGHT`），驱动把每个 chunk（默认 4 逻辑行 × 1280 列）**转置成物理竖条**后一次 `draw_bitmap` 推送
- 字节序：rg_display 对 `RG_PIXEL_565_LE` 源做过一次交换 → 到这里是 565 大端；DSI 面板要小端，驱动里 `bswap16` 换回
- `draw_bitmap` 在 `use_dma2d=1` 时异步，忙时返回 `ESP_ERR_INVALID_STATE` → 驱动内重试（正常现象，不是故障）
- P3 计划：改用 P4 的 **PPA 硬件 SRM**（`#include "driver/ppa.h"`，IDF 5.5 自带 `esp_driver_ppa`）做旋转+缩放一次完成，替掉 CPU 转置

## 三、构建期踩过的坑（本 fork 特有）

| 现象 | 根因 | 修法 |
|---|---|---|
| 编到 `rg_input.c` 报 legacy ADC API 不存在 | 新建了 `targets/tab5/` 但**忘了在 `components/retro-go/config.h` 加 `RG_TARGET_TAB5` 分发分支** → 回落到 ODROID-GO 配置（它定义 `RG_BATTERY_DRIVER 1`） | 加分发分支。**排查信号：日志里的 `#warning "No target defined. Defaulting to ODROID-GO."`** |
| `fatal error: lvgl.h` | `bsp/m5stack_tab5.h` umbrella 头会拉 lvgl.h；`BSP_CONFIG_NO_GRAPHIC_LIB=1` 在 BSP 里是 **PRIVATE** 定义，不传给使用方 | 驱动不再 include 该头；并在 `components/retro-go/CMakeLists.txt` 里自己加 `-DBSP_CONFIG_NO_GRAPHIC_LIB=1` |
| BSP 找不到 | `base.cmake` 的 `EXTRA_COMPONENT_DIRS` 只指 repo 自己的 `components/` | 追加 `../vendor` 与 `../vendor/managed_components`（与自检固件共享一份 BSP） |

## 四、产物与刷机

- 镜像：`retro-go-p4/retro-go_<ver>_tab5.img`（合并镜像：bootloader@0x2000 / 分区表@0x8000 / launcher@0x10000 / gbsp@0x100000）
- 分区：launcher 960KB、gbsp 704KB（**gbsp 只剩 ~47KB 余量**，再加核心要重排分区表）
- 刷机：`sh tools/flash-retro-go.sh`（整片从 0x0 写，首次 `--force`，自带回读校验）
- 归档镜像：`dist/retro-go-p1-displayonly.img`（sha256 `849ec1304cb580f8…`）、`dist/retro-go-p2-touch.img`
- 烧录前先确认 `/dev/cu.usbmodem101` 是 Tab5（只有一个原生 USB 串口）

## 五、阶段状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| P1 | tab5 target + BSP 显示 → 屏幕出 retro-go 菜单 | **构建通过**（tag v0.3，镜像 `dist/retro-go-p1-displayonly.img`），等真机刷机验证 |
| P2 | ST7123 触摸 → 虚拟手柄 | 配置/构建通过（tag v0.6，镜像 `dist/retro-go-p2-touch.img`），等真机验证方向变换 |
| P3 | gbsp 跑 GBA：3× 整数缩放 720×480 居中 + 量帧率 | 未开始（PPA API 已确认可用） |
| P4 | ES8388 音频（新 I2S API） | 未开始，明确放最后 |

### 触摸键位的硬约束（改键位前必读）

虚拟按键**不得压在游戏画面上**。按 3× 整数缩放算，240×160 → 720×480 居中，
游戏区占逻辑坐标 `x[280,1000) y[120,600)`，四周空白 margin 为
**左 280 / 右 280 / 上 120 / 下 120 px**。`targets/tab5/config.h` 的
`RG_GAMEPAD_TOUCH_MAP` 全部落在 margin 内（最小间隙 8px），预览图
`docs/touch-layout-p2.png`（用 `tools/`-外的临时脚本生成，改键位后重绘）。

推论（已在 target config 里锁死）：
- 游戏缩放必须是 **ZOOM + custom_zoom=3**（`RG_DISPLAY_DEFAULT_SCALING/CUSTOM_ZOOM`）；
  `FIT` → 1080×720（上下零 margin）、`FULL` → 拉满 1280×720，两种都会让按键压画面。
- 上游把 `custom_zoom` 硬夹在 2.0，已改为可被 target 覆盖（`RG_DISPLAY_MAX_CUSTOM_ZOOM`），
  tab5 放开到 4.0。
- launcher 菜单是全屏 UI，那时按键压在菜单上是正常且必需的（要能选游戏）。

## 六、真机调试时的检查顺序

1. 刷 P1 镜像 → 看是否出菜单（若黑屏：先看日志有没有 `Tab5 DSI ready` 与 `backlight set to 80%`，区分"通路没起来"和"屏没亮"）
2. 刷 P2 镜像 → 点屏幕四角/中心，看选中项是否跟随；方向不对改 `RG_TOUCH_LOGICAL_FROM_PHYS`（rg_input.c）或触摸 config 的 mirror 标志
3. 插 SD 卡（`/retro-go/roms/`、`/retro-go/bios/gba_bios.bin`）→ 选 ROM 进 gbsp → 看帧率
