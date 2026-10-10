# HANDOFF · 0.4.9 单 app（横竖切换）实施交接

**目标（用户 2026-10-10 拍板）**：0.4.9 = 一个 app 装全部（启动器 + gbsp + retro-core 多核 + 横竖两方向）。
对外卖点是"**支持横竖切换**"；单 app 只算内部实现。**一次做完 S1→S4，只要结果，不逐期确认。**
设计依据：`docs/SPEC-0.4.9-SINGLE-APP.md`（含实测数字、工作项 W1–W16、验收判据）。

## 已确认的关键事实（别再重新调研）

| 事实 | 值/位置 |
|---|---|
| 单 app 形态 | **已实现**，开关 `-DRG_SINGLE_APP=1`：`launcher/CMakeLists.txt`、`launcher/components/gbsp-core/`、`retro-core-main/`、`base.cmake:43`、`rg_tool.py --single-app` |
| 单 app 构建入口 | `bash tools/build-tab5-skin.sh single-app`（内部 = `rg_tool.py --target tab5 --no-networking --single-app release launcher gbsp`） |
| 单 app 的进/出核心 | NVS `rgapp/core` 待续标志 + 重启（`rg_system.c:172-230`、分发器 `launcher/main/main.c:554-576`） |
| **0.4.8 单 app 分区表（历史真源，从镜像 0x8000 解出）** | `nvs 16K@0x9000 / otadata 8K@0xd000 / phy 4K@0xf000 / **launcher app ota_0 1984K@0x10000**`（总 2MB，**只有一个 app 槽**） |
| 0.4.8 多 app 分区表 | `launcher 960K@0x10000 / retro-core 1472K@0x100000 / gbsp 1216K@0x270000` |
| 单 app 槽大小 | 由 `rg_tool.py` **按二进制体积自动向上取整扩容**（不存在"槽不够"问题）⇒ 0.4.9 沿用 0.4.8 表形状即可，**从 0.4.8 升级不用动分区表** |
| flash | 16MB；历史单 app 镜像 1.3–2.0MB（含一份驱动）⇒ 含两份驱动预计 ~2.2MB，余量充裕 |
| **可见区两方向相同** | 横 `1280×720` 挖边距 280/120 = **720×480**；竖 `720×1280` 挖下方 800 = **720×480** ⇒ **GUI/菜单/皮肤布局零改动** |
| 物理坐标空间 | 两方向键位表都在 **720×1280** 逻辑空间（`touch_layout.h`）⇒ 只有"选哪张表 + 驱动映射"不同 |
| 方向耦合面（编译期） | `RG_TAB5_ORIENTATION` 13 处 / 2 文件：`targets/tab5/config.h`（3 处 `#if`：97–192、229–243）、`targets/tab5/touch_layout.h`（2 处 `#if`） |
| 几何常量用量 | `RG_SCREEN_WIDTH/HEIGHT` 94 处；非 target 文件只有 3 个：`rg_display.c` 13、`rg_touch_overlay.c` 11、`rg_touch_skin.c 4`；驱动内各 2 处（日志 + 窗口边界检查） |
| **运行时几何真正的字段（只有这些差异）** | drivers 号(2/3)、逻辑 w×h(1280×720 / 720×1280)、visible_area(+NS)、`max_win_h`(576/620)、`lock_scaling`(1/0)、`default/max zoom`(4.0/16.0)、`LCD_BUFFER_LENGTH` 行数(16/32)、键位表、触摸映射 |
| 两份驱动 | `mipi_dsi_tab5.h`(71KB 横) / `mipi_dsi_tab5_p.h`(57KB 竖)，同名一整套 `static` ⇒ **各自包一个 TU 即可共存，驱动内部一行不改**（已实测通过） |
| 两份驱动同编的 RAM 代价 | **零**：帧缓冲都是 `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`，在 `lcd_init()` 时才分配；分发只调生效那一份 ⇒ 只有一份帧缓冲。仅多占 ~0.2MB flash 代码 |
| 组件 CMake | `COMPONENT_SRCDIRS "." drivers/audio fonts libs/netplay libs/lodepng` —— **不含 `drivers/display`** ⇒ 包装 `.c` 必须放**组件根**（`components/retro-go/tab5_{l,p}_api.c`） |
| 跨 TU 依赖 | 竖屏驱动调用 `rg_display.c` 的 `rg_display_push_failed()`（非 static，但过去靠"同 TU 隐式声明"）⇒ 包装层补一条声明即可 |
| 驱动对方向宏的依赖 | 两份驱动**不引用** `RG_TAB5_ORIENTATION`、无 `#error` 守卫 ⇒ 任意方向构建都安全 |
| 缩放锁定 | `RG_DISPLAY_LOCK_SCALING` 只有 2 处：`rg_display.c:1045`、`rg_gui.c:1806` |
| 方向真值（现状） | 运行分区标签（`rg_orient_running()`）；**单 app 下标签不带方向 ⇒ 改为 NVS 权威** |
| 存档 | SD（`RG_BASE_PATH_SAVES`）⇒ 换分区表不丢存档（只丢 NVS 少量设置） |
| 方向设置 | `ui/orient`（SD `ui.json`）；UI 已有（首启弹窗 + 设置页）⇒ **本次零 UI 改动** |

## 实施计划与进度

### Step A · 两驱动同编 + 运行时分发（不改行为）—— ✅ **已完成并编译验证**
- [x] A1 `drivers/display/tab5_p_api.{c,h}`：竖屏驱动包成 `tab5p_*`（`.c` 放**组件根**，见上表 CMake 事实）
- [x] A2 `drivers/display/tab5_l_api.{c,h}`：横屏驱动包成 `tab5l_*`
- [x] A3 `drivers/display/tab5_dispatch.h`：`lcd_init/…/send_buffer` 定义成 static inline 分发器（按 `rg_orient_active()` 选）⇒ `rg_display.c` 调用点**一字未改**
- [x] A4 `rg_display.c` 的 `#elif RG_SCREEN_DRIVER == 2 || == 3` → include 分发层（0/99/else 分支未动）
- [x] A5 编译验证：双 app 竖屏档（**两份驱动同编**）rc=0、0 错误、`launcher.bin` 858KB；单 app 档已构建
- [x] A6 `rg_orient.c` 加 NVS 权威方向（`rg_orient_active/active_set/sync_from_settings`）+ 单 app 下 `rg_orient_running()` 复用；`rg_orient_set()` 在单 app 下同步写 NVS；`rg_orient_restart_into()` 单 app 分支 = 写 NVS + 重启（双 app 路径原样保留）

### Step B · 几何运行时化（编译器驱动）
- [ ] B1 `drivers/display/tab5_geom.h`：两张方向几何表 + `rg_geom()`（字段见上表"运行时几何真正的字段"）
- [ ] B2 把 `RG_SCREEN_WIDTH/HEIGHT` 等**改名成方向专用**（`…_L / …_P`）⇒ 让编译器逐个报出使用点，**不靠人找**
- [ ] B3 `rg_display_get_width/height()` 返回当前方向值（注意 3 个按宏定长的静态数组 `map_viewport_to_source_x/y`、`screen_line_checksum` ⇒ 按**两方向最大值**分配 + 运行时计数）
- [ ] B4 `RG_DISPLAY_LOCK_SCALING` 2 处 → 运行时
- [ ] B5 `touch_layout.h` 两张键位表 → 运行时选表（+ 竖屏的触摸映射覆盖）
- [ ] B6 驱动内 2 处 `RG_SCREEN_WIDTH/HEIGHT` → 运行时；`LCD_BUFFER_LENGTH` 行数 16/32 → 运行时（缓冲区按最大值 `32×720` 分配）

### Step C · 方向真值与切换
- [x] C1 NVS `rgapp/orient` 权威值（显示 init 之前可读，惰性 + 缓存）—— 已随 A6 落地
- [x] C2 迁移：NVS 无值而 SD `ui.json` 有值 ⇒ 采纳（差异时重启一次，自终止）—— `rg_orient_sync_from_settings()`（需在存储层起来后调用）
- [x] C3 单 app 下 `rg_orient_running()` = NVS；`rg_orient_restart_into()` = 写 NVS + 重启
- [ ] C4 `rg_system.c:265-289` `rg_orient_app_label` 分支用 `#if RG_SINGLE_APP` 分流（双 app 路径不动）
- [ ] C5 `rg_orient_sync_from_settings()` 的调用点接进 `rg_system_init()`（存储层之后）
- [ ] C6 兜底：显示 init 失败 ⇒ 回落另一方向 + 记日志

### Step D · 分区表 / 工具 / 发布
- [x] D1 分区表：**沿用 0.4.8 单 app 表形状**（`rg_tool.py --single-app` 已自动生成 + 按体积扩容）——不需要新造 CSV
- [ ] D2 `rg_tool.py` merged 内嵌表 → 真源（顺带修 P3；单 app 形态下风险自然消失）
- [ ] D3 门禁脚本（`find -newer` + sha256 + 单 app 表校验）
- [ ] D4 真机验收（SPEC §5 六条）
- [ ] D5 发布物 + 说明（M5Burner 一个包；从 0.4.8 升级**不用改分区表**，存档在 SD 不受影响）

## 纪律（沿本项目既有口径）
- 长任务后台跑 + 日志落 `.buildlogs/`；同一时刻只允许一个构建 / 一个串口读端。
- 刷机前 marker + `find -newer` + sha256 断言"刷的就是刚编的"；刷机带 `DEV_PORT=/dev/cu.usbmodem101`（脚本默认 1101 是错的）。
- **钩子/实验开关交付前一律归零**；实验期不出发行镜像。
- 一次只改一个变量；被否就整笔回退到已验证基线（竖 `dist/orient-r3-portrait` / 横 `dist/orient-r4-landscape`）。
- 皮肤底图横屏化属 GUI 改动 ⇒ 本次**不做**（用户已定）。

## 回退路径（随时可用）
`DEV_PORT=/dev/cu.usbmodem101 bash tools/flash-tab5-dual.sh dist/orient-r3-portrait/merged-portrait.img dist/orient-r4-landscape/`
（竖横双刷；把引导槽切回竖屏用 `otatool.py --port $DEV_PORT switch_ota_partition --name launcher`）

## 本轮进展（2026-10-10 夜，真机已验）

**已经完成的（都有真机/构建实证）**
- Step A 双驱动同编 + 运行时分发（`tab5_dispatch.h` 按 `rg_orient_active()` 转发 8 个 `lcd_*`；`lcd_init()` 里**粘性锁定**后端，写 NVS 只影响下次启动 ⇒ 改方向期间不会新旧后端错配黑屏）。
- Step B 几何运行时化：新建 `targets/tab5/geom.h`（两张方向表，值逐字照抄原 `config.h` 分支）；`config.h` 的
  几何宏改成读 `rg_geom()` 的表达式 ⇒ **94 处调用点一字未改**；两份驱动行缓冲按两方向最大值分配。
- **按方向编死的"资产表"全部运行时化**（这是本轮真正的坑，共 2 处）：
  1. 键位表：`rg_input.c` 启动时按方向把该用的那张 `memcpy` 进可变数组 `keymap_touch[]`
     —— 皮肤布局的**唯一真源就是键位表**（`rg_touch_skin.c`），表选错=面板/按键/命中区全错。
  2. 切换键坐标：`RG_TAB5_SWAP_X/Y` 改运行时三元表达式（`#include "rg_orient.h"`）。
  全项目扫描确认**无第三处**；仅剩 2 处有意保留（`RG_SCREEN_DRIVER` 编译期、键位表主/备输入）。
- 单 app 方向真值：NVS `rgapp/orient` 为权威（显示 init 早于 SD 挂载）；`rg_orient_restart_into()` 单 app = 写 NVS + 重启；
  `rg_orient_sync_from_settings()` 接进 `rg_system_init()`（老版 SD 选择一次性迁移）；单 app 下 `rg_orient_app_label()` 不加 `_l` 后缀。
- 单 app 分区表 P3 修复：`rg_tool.py build_image()` 加 `single_app` ⇒ 内嵌表**不再含 `_l` 槽**、app 槽 1984K；
  `tools/check-single-app-image.py` 门禁 PASS（0xFF 扫描算真实余量）。
- **交付钩子审计通过**：`RG_TAB5_PPA_MODE=0`、`RG_GBA_DIAG=0`、`RG_TEST_KEYS_DEVICE=""`、`RG_TEST_NO_SAVE=0`、`RG_TEST_MUTE=0`。
- 真机实证（单 app 一份镜像、运行时切方向）：竖屏 `linear 1:1 map` + 稳态 `rows=32.0 max=32`；
  横屏 `90CW map` + 稳态 `rows=28.0 max=28`（与 r4 横屏基线**逐字一致**）；两种方向均零 panic。

**待办**
- 横屏 GUI 视觉效果由用户真机确认（切换键位置本轮已修，待刷入验证）。
- 双 app 三形态回归构建（B7 那轮为腾出构建位被撤，需重跑一遍）。
- 皮肤**底图**横屏化（用户本轮明确"先不做"，属装饰层，不影响布局/命中）。
- 发布口径（对外只讲"支持横竖切换"）、双语 CHANGELOG、M5Burner 上传。

## 真机验收（2026-10-10 夜，用户确认）

- **用户验收通过**：横屏切换键已回到 L 与 R 正中（"这次对了"）。
- 验收用镜像：`retro-go_v0.4.8-12-gf17e9-dirty_tab5-single.img`
  - sha256 `d8839ef5a3f6056aa785de38d70f37b414a00e38e36a5adf6ff9dd60a8566615`
  - 单 app 分区表门禁 PASS：`launcher @0x10000` 槽 1984K，实际用 **1952.2K（余 31.8 KB，1.6%）**
  - 构建：`BUILD END rc=0`、0 编译错误；交付钩子审计通过（PPA/诊断/测试键/静音全关）
  - git HEAD 短号 `f17e957`（**未 commit**：本轮改动仍在工作区）
- ⚠ 记录两处**日志标签与实际不符**（不是缺陷，避免后人误读）：
  1. `V5-swapfix-portrait.log` 里其实是**横屏**启动 —— 因为 NVS 里 `rgapp/orient` 仍是上一次测试写入的 1，
     而刷机不擦 NVS ⇒ 刷完直接进横屏。这恰好证明"方向真值在 NVS、跨刷机保留"这条设计成立。
  2. `V4/` 等日志里 `rows=28.0 max=28` 是**横屏稳态**值；`max` 偶发大值（48/182/301/395/480）是开机整屏推送，
     不是分块超限 —— 判超限只看**稳态 rows**（横屏 28 / 竖屏 32）。
- 结论：**0.4.9 单 app 的形状（一份镜像 + 运行时方向 + 运行时分发）已在真机跑通并被用户接受。**

## ⚠ 更正（2026-10-10 夜）：分区表变化与升级路径

**作废的旧结论**：早前写的"从 0.4.8 升级不用动分区表" ← 那是拿**历史单 app 表**（0.4.8 时期的
`*_tab5-single.img`）推的，**对 M5Burner 上实际发布的 0.4.8 不成立**。

**实测对比**（本机镜像解表）：
| 形态 | 分区表 |
|---|---|
| 0.4.8 多 app（M5Burner 上线的形态；本机 `dist/*` 横屏档） | `launcher 960K@0x10000` + `retro-core 1472K@0x100000` + `gbsp 1280K@0x270000`（+ 三个 `_l` 槽，未发布） |
| **0.4.9 单 app（本文件交付形态）** | `launcher app 1984K@0x10000`（一个大槽） |

⇒ **分区表确实变了**（三槽 → 一槽）。

**但升级路径不受影响**：M5Burner 的包是 **merged 全镜像、整片从 0x0 写入**
（见 `dist/m5burner-0.3/README_M5Burner.md`：bootloader@0x2000 + 分区表@0x8000 + app…）
⇒ 刷 0.4.9 时分区表被整体覆盖，用户不需要任何手工操作。

**唯一要避免的**：手工用 esptool 只刷 app 分区（`0x10000`）而不刷表 —— 新的 app 有 1952KB，
原 960K 槽装不下。发布说明里要写明"请用整包（M5Burner 安装）"。
