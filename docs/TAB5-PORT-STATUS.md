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
| P2.5 | 虚拟手柄**可视层**（标签/透明度/按下反馈/隐藏） | **真机已验证**：13 键标识可见、5 档透明度可调且存 NVS、按下反馈正常、进出游戏黑屏时长已回到正常（建层实测 ~200ms/次，见 §94）。保底手势（隐藏后左上角长按 1.2s）待手动验证 |
| P3 | gbsp 跑 GBA：3× 整数缩放 720×480 居中 + 量帧率 | 未开始（PPA API 已确认可用） |
| P4 | ES8388 音频（新 I2S API） | 未开始，明确放最后 |

### 触摸键位的硬约束（改键位前必读）

虚拟按键**不得压在游戏画面上**。按 3× 整数缩放算，240×160 → 720×480 居中，
游戏区占逻辑坐标 `x[280,1000) y[120,600)`，四周空白 margin 为
**左 280 / 右 280 / 上 120 / 下 120 px**。键位表 `targets/tab5/touch_layout.h`
（P2.5 起从 config.h 抽出来成单一数据源，命中判定/绘制/PC 预览共用）全部落在 margin 内
（最小间隙 8px），预览图 `docs/touch-layout-p2.png`（用 `tools/`-外的临时脚本生成，改键位后重绘）。

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

## 七、虚拟手柄可视层（P2.5）

**为什么要有**：触摸屏没有实体键，看不见命中区 = 用户不知道往哪点。可视层把键位画出来。

| 文件 | 职责 |
|---|---|
| `targets/tab5/touch_layout.h` | 键位表**单一数据源**（位置/尺寸/键），命中判定与绘制都读它 |
| `rg_touch_overlay.{h,c}` | 可视层本体：标签 / 透明度 / 按下反馈（目标无关，任何 target 可用） |
| `rg_input.c` | 命中判定 + 保底手势（见下） |
| `drivers/display/mipi_dsi_tab5.h` | 在**推给面板前**合成（CPU 转置路径与 PPA 直写路径各一处调用） |
| `rg_gui.c` | Options 菜单两项：Touch buttons（开关）/ Touch opacity（档位） |
| `tools/preview-touch-overlay.py` | PC 设计稿（改颜色/标签规则后要同步） |
| `tools/build_sdl2_mac.sh overlay` | 出 `overlay-preview`：宿主按 tab5 分辨率+真键位表渲染，可定点出图 |

**为什么在显示驱动里合成**：GUI 是立即模式，画在别处会被游戏帧或菜单重绘覆盖 → 实机表现为按键闪烁（`27b44b4` 修过一次）。

**关键实现取舍**
- **一次性预渲染掩码**：init 时把每个按键烘成 `覆盖率<<3 | 角色`（角色：1 边框 / 2 填充 / 3 文字），每帧只查表混合。整屏约 123KB，比存 RGB 位图省 6 倍；4×4 超采样顺手白拿抗锯齿，每帧零开销。
- **透明度分层**：填充 0.50 / 边框 0.90 / 文字 1.00。三层同透明度的话低档位下字和底一起变淡 → 20% 档位看不清；分层后 20% 仍认得出。
- **混合权重必须 ≤ 6 位**（`AW_BITS`）：见经验沉淀 §90，用 8 位权重 + `0xF81F` 掩码 + `>>8` 那个流传写法，蓝通道会进位撞进红字段，实测最大偏差 24/31（A 键红边框发紫）。
- **按下反馈有 120ms 保底保持**（`RG_OVERLAY_PRESS_LINGER_MS`）：不点就闪一下的话，快速点按看不见反馈。
- **隐藏后必须有出路**：左上角 100×100 长按 1.2s 注入 `RG_KEY_MENU`（`rg_input.c`）。没有它，关掉按键就再也进不了菜单。

**PC 侧怎么验的**（不刷机往返）
```sh
sh tools/build_sdl2_mac.sh overlay
RG_SDL2_SHOT=/tmp/out.png:3:40:LEFT|A   ./build-sdl2/overlay-preview   # 路径:延迟秒:透明度:强制按下的键
```
13 个按键的边框/填充颜色与逐通道精确混合模型逐个对齐（11 个 Δ=0，另 2 个采样点落在字形上）、
四档透明度单调递减、按下态变白边、档位重启后沿用 —— 都用 PIL 数值比对确认，不依赖视觉模型判断。


## 八、中文支持（CJK 字库 + 中文菜单）

**目标**：中文 ROM 文件名与界面菜单都能正常显示，且不依赖 SD 卡（用户便捷至上）。

### 字库

| 项 | 内容 |
|---|---|
| 来源 | 缝合像素字体 Fusion Pixel 12px（TakWolf，OFL-1.1，授权文件 `tools/CJK-FONT-LICENSE-OFL.txt`） |
| 覆盖 | **3773 字形**：GB2312 一级 3755 字**一个不缺** + 常用标点（、。《》「」！？…—·） |
| 规格 | 12×12 点阵，二进制格式 `RGF1`（magic + cell_w/h + count + 索引偏移 + 字形偏移；索引为升序码位，字形每行 2 字节、**MSB 在左**） |
| 生成 | `tools/gen_cjk_font.py`（BDF → 二进制）→ `retro-go-p4/assets/cjk12.bin`（103KB） |
| 存储 | **独立 flash 分区** `cjkfont`（type=data, subtype=0x40, 128KB），由 `rg_tool.py` 的 `build_image()` 打包 |

**为什么放 flash 分区而不是 SD 卡或 app 二进制**：放 app 里 13 个模拟器各带一份 = 1.3MB（app 分区只有 960KB/1MB，塞不下）；
放 SD 卡要用户手动拷、换卡就丢。分区 + `esp_partition_mmap` = 一份全 app 共享、**零加载时间**（不走 PSRAM 拷贝）、换卡不影响。

### 设备端接入

| 文件 | 职责 |
|---|---|
| `rg_cjk.{h,c}` | 找分区 → mmap → 校验头部 → 升序索引二分查找 → 缩放填充；缺字返回 0 让调用方回退方块 |
| `rg_gui.c` `get_glyph()` | 在"缺字方块"分支**之前**插一层 CJK 查询 |

**关键：位图约定**（唯一权威是 `get_glyph()` 里拉丁字形的解码 `row |= (1 << (xOffset + x))`）：
- 输出 **bit 0 = 最左像素**；
- 行内只放**原生宽度**（12 列）的像素，**横向缩放由调用方做**（`sx = x * glyph_width / glyph_scaled`）；
- **纵向要铺满 `points` 行**（`get_glyph` 只对拉丁字形做纵向缩放）。

汉字宽度按"一个汉字占一个 em"返回（`font->height`），于是 GUI 现有的等比缩放、宽度测量、居中/右对齐、
**截断不劈字**（按码位迭代）全部无需改动。

### FATFS 编码（必须与字库同步）

`CONFIG_FATFS_API_ENCODING_UTF_8=y` + `CONFIG_FATFS_CODEPAGE_936=y`，**target 的 sdkconfig 和各 app 已有的
sdkconfig 都要改**（否则改动静默失效，见经验沉淀 §99）。

### 菜单中文化

`translations.h` 的每个条目加一列 `[RG_LANG_ZH]`（**197 条全覆盖**），`language_names` 加 `"中文"`，
`rg_localization.h` 加 `RG_LANG_ZH` 枚举。**语言切换菜单项、NVS 持久化本来就是现成的**（`rg_gui.c` 的 `language_cb`）。
默认仍为英文（安全：万一中文渲染异常，菜单还能看懂）；用户可在 `Options → Language → 中文` 一键切换。

**真机验证**：日志出现 `rg_cjk_init: cjk: font ready, 3773 glyphs, 12x12 cells, 128 KB mmap'd`；
切换后 `rg_gui_set_language_id: Language set to: 中文 (2)`，重启后保持。

### 顺带修的两个 bug

1. **封面路径少一个点**（`launcher/main/gui.c`）：`rg_extension()` 返回**不含点**的扩展名，原代码只处理了"有扩展名"的情况；
   **目录项没有扩展名** → 拼出 `xxxpng` → 文件夹类条目的封面永远加载不到。已修（无扩展名时补 `.png`）。
   命名约定：ROM `X.gba` → 封面 `X.png`；目录 `Y` → 封面 `Y.png`。
2. **路径标题按字节截断会劈字**：新增 `rg_path_tail()`（`rg_utils.c`），按 **UTF-8 码位边界**从**尾部**截断
   （末尾才是当前所在目录），截断点尽量落在 `/` 之后，前面加 `…`；并改用 `rg_relpath()` 去掉 `/sd` 前缀。

## 九、待解决：GBA 帧显示性能 / DSI 报错（2026-09-24 发现，未修）

**现象**：个别 GBA 游戏（实测《宝可梦弹珠台》中文版）屏幕看起来"卡死在启动画面"，但**模拟器本身完全正常**：

```
[info] app_main: GBA
[info] app_main: reset_gba
[info] app_main: emulation loop
[debug] ... BUSY:41%, FPS:60 (30+30+0)     ← 持续稳定 60 FPS，无 panic、无 watchdog
```

**真凶在显示管线**（同一次抓取的 300 秒日志）：

```
E (5878) lcd.dsi: dpi_panel_draw_bitmap(547): previous draw operation is not finished     ← 126 条
[info] tab5_perf_report: PERF: display=431.17ms/1000ms (transpose=430.88 submit=0.28) blocks=313
```

- **每秒 431ms 花在 CPU 转置**，而 `submit` 只要 0.28ms —— 瓶颈完全在转置。
- 12 条 PERF 采样里 transpose 从 47ms 递增到 431ms（平均 157ms），**持续恶化**。
- DSI 面板持续报"上一次绘制还没完成" = 绘制速度跟不上，**帧被丢弃 → 屏幕停在旧帧**，
  看起来就像游戏卡死（实际游戏在跑，按键也在响应，只是**看不见**）。

**待查方向**：
1. 为什么这里走的是 **CPU 转置**而不是 PPA/CW90 硬件直写路径？（`mipi_dsi_tab5.h` 里两条路径都有调用点）
2. `dpi_panel_draw_bitmap` 的"上一次未完成"是节流缺失还是面板刷新率/带宽不足？
3. 为什么其他游戏不明显 —— 是否与游戏帧率/画面变化量有关，还是所有游戏都有、只是没注意？

**临时结论**：这不是游戏 ROM 的问题（ROM 头部与能正常运行的《火纹》逐字节一致，8MB vs 16MB 都能跑），
也不是模拟器内核问题，而是**我们自己的显示通路**要优化。
