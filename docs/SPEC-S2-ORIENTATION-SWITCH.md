# SPEC S2 — Tab5 横竖屏切换（开机选择页 + 重启确认 + NVS）

状态：**设计稿**（等用户确认后再动实现）。写于 2026-10-09 凌晨，横屏 r6 已上机。
作者约束来源：用户 2026-10-08 拍板 ②「首次开机选择页 + 二次选择 + 弹框告知重启/必须存档/点 OK 才继续」。

## 0. 用户最终要求（2026-10-09 01:10，用户逐字，**优先级最高**）

> 我最后说一下对横屏切换的功能要求，第一次使用，弹窗，让用户选择横还是竖，要说明设置哪里可以重选。默认是竖屏。

拆成可验收的条目：

| # | 要求 | 落地方式 |
|---|---|---|
| R1 | **第一次使用弹窗** | NVS 无 `orient` 记录 ⇒ 启动后（进入 launcher 主界面之前）弹一次选择页 |
| R2 | 弹窗里二选一：横 / 竖 | 两个大按钮（LANSCAPE / PORTRAIT），点哪个存哪个 |
| R3 | **弹窗里必须写明"以后在哪儿重选"** | 文案固定包含一行：`You can change this later in Settings → Screen orientation.`（中英各一行，跟现有 UI 语言设置走） |
| R4 | **默认竖屏** | ① 出厂槽 = 竖屏（未选择时设备就跑竖屏）② 弹窗里"横屏"按钮只是**写入选择**，不预置任何横屏状态 ③ 用户什么都不点就继续用 ⇒ 保持竖屏 |
| R5 | 以后再选 | 设置菜单里的 `Screen orientation` 项 ⇒ 同一个选择页（带"会重启/先存档"确认） |

**与 §5 原设计的关系**：R1–R5 覆盖原 §5，原 §5 里的"重启确认弹框"仍保留（从设置菜单二次选择时才需要重启确认；首次选择时因为设备当前就在默认竖屏、且用户尚未开始用，只需提示"重启生效"）。

---

## 1. 目标与非目标

**要**：机内切换横屏/竖屏；首次开机（NVS 无记录）出选择页；之后再选要弹框告知"会重启、必须存档、点 OK 才继续"；选择落 NVS，重启后生效。

**不要**：不把两份显示驱动合并成一份（见 §2 的取舍）；不改竖屏既有视觉与布局（用户已认可的基线）；不引入网络/OTA 服务器。

## 2. 关键取舍：为什么用"双 app 槽"而不是"合并驱动"

| 方案 | 工作量 | 风险 | 结论 |
|---|---|---|---|
| A. 两份驱动合并进同一镜像 + 运行期虚表切换 | 大（两驱动 **差 427 行**，842 vs 975） | 高：要动竖屏稳定基线；驱动内 static 函数名冲突需逐个包一层；改完横竖两条路径都要重测 | ✗ 否决 |
| B. 两份 app 镜像 + 引导器按 NVS 选槽（本 SPEC） | 中（分区表 + 两槽烧写 + 一个小的切换 API） | 低：**两份驱动一行都不改**，各自仍是编译期定向；切换只影响"引导器加载哪个 app" | ✓ 采用 |

B 的关键性质：**切换发生在"引导之前"**，而不是在驱动初始化之后 ⇒ 不需要运行期驱动抽象。
代价：两份 app 分区（各 ~3.8MB，Tab5 16MB flash 够用）+ 切一次要重启（**用户已明确接受重启**，且要求弹框告知）。

**实现期发现（2026-10-09，重要）**：本工程**本来就有"重启自己 + NVS 待续标志"来切 app 的机制** ——
`launcher/main/main.c` 的 `RG_SINGLE_APP` 分发段 + `rg_system.c` 的 `update_boot_config` +
`rg_system_switch_app()`（单 app 形态 = 菜单与核心编在同一 app，靠重启 + NVS 标志决定这次起哪一套）。
⇒ 方向切换**必须复用这套范式**，不要另造一套切换协议：方向 = 选择"起哪一套 app 分区"，
与"起 launcher 还是起核心"是同一类问题。落点已按此设计：`rg_orient_restart_into()` 只做
"写 NVS + `esp_ota_set_boot_partition` + 重启"，与现成机制同构。

## 3. 分区与引导

**实测真相（2026-10-09）**：本工程不是 `factory/ota_0` 那套，而是**三 app 分区 + 按标签查找**：

```
nvs,          data, nvs,     36864,   16384
otadata,      data, ota,     53248,   8192
phy_init,     data, phy,     61440,   4096
launcher,     app,  ota_0,   65536,   983040      ← 竖屏（现状，标签一个字都不改）
retro-core,   app,  ota_1,   1048576, 1507328
gbsp,         app,  ota_2,   2555904, 1245184
launcher_l,   app,  ota_3,   3801088, 983040      ← 横屏：同三件套 + `_l` 后缀
retro-core_l, app,  ota_4,   4784128, 1507328
gbsp_l,       app,  ota_5,   6291456, 1245184
```
合计 7,536,640 B ≈ 7.2MB（16MB flash 够）✓；**nvs / otadata / phy_init 的偏移与现状完全一致** ⇒ 用户的设置与存档不受影响 ✓。

关键事实（决定了实现方式）：
1. **app 是按标签找的**：`rg_system.c:1133` `app_is_available()` = `esp_partition_find_first(APP, ANY, app)`，
   `rg_system.c:264-266` 切引导槽也是同一套（`strncmp(label, partition, 16)`）⇒ **标签 ≤16 字符**（`retro-core_l` = 12 ✓）。
2. 所以**方向 = 标签后缀**：正在跑的分区标签以 `_l` 结尾 ⇒ 横屏；不带 ⇒ 竖屏。
   `rg_orient_get()` 的"当前方向"由此**实测得到**（不依赖 NVS，避免"记录与事实不一致"）。
3. ⇒ **所有拼 app 标签的地方都要过一遍方向后缀**（launcher 里列核心、`rg_system_switch_app` 的调用点等），
   否则横屏下点"GB"会启动**竖屏**的 retro-core（会闪一下竖屏布局）。这是本方案唯一的"必须全面"的改动面。

引导槽切换由 `rg_orient_restart_into()` 完成：取当前 app 标签 → 加/去 `_l` → `esp_ota_set_boot_partition` → 写 NVS → 重启。
（与工程既有范式同构：`esp_ota_set_boot_partition` + 重启，见 §3 的实现期发现。）

## 3.5 落地状态（2026-10-09：**已完工，真机双向验证通过**）

| 用户要求（§0） | 落地 | 实机证据 |
|---|---|---|
| R1 首次使用弹窗 | ✅ launcher `retro_loop()` 首启钩子 | 用户实机看到 ✓ |
| R2 横/竖二选一 | ✅ `rg_gui_dialog_orientation()` | `orient: saved choice Landscape/Portrait` ✓ |
| R3 写明以后在哪重选 | ✅ 对话框两行 MESSAGE | `Settings > Screen orientation` ✓ |
| R4 默认竖屏 | ✅ 结构性：未选择就跑竖屏槽 `launcher` | 首启即竖屏 ✓ |
| R5 设置里可重选 | ✅ 设置项 `Screen orientation` | 用户从设置切换成功 ✓ |
| 切换真的生效 | ✅ `esp_ota_set_boot_partition` + 重启 | 横→`90CW map` / 竖→`linear 1:1 map` ✓ |
| 选择被记住、弹窗只出一次 | ✅ NVS `ui/orient` | 断电重开后保持 ✓ |
| 重启过渡（不再是"蓝屏"） | ✅ `rg_gui_draw_restart_notice()` | 用户实机看到 `Restarting...` ✓ |

**已知美容残留（用户已接受，不再处理）**：过渡屏之后、新固件接管显示之前，面板仍会闪一瞬蓝。
成因是 DSI/面板复位期的状态，不是本工程绘制的画面（`rst:0xc (SW_CPU_RESET)` + 无 panic 已证）。

**配套门禁**：`tools/check-dual-partition.py` —— 分区表三处副本（真源 CSV / `rg_tool.py` 元组 /
merged 镜像 0x8000 内嵌表）必须一致，已接进 `tools/flash-tab5-dual.sh` 作为动设备前的前置检查。
（为什么要门禁：见 ESP32-经验沉淀.md §233 —— `retro-go-p4/partitions.csv` 是**产物**不是真源。）
**实际生效的槽尺寸**（以 `tools/partitions-dual-tab5.csv` 为准，横屏槽各留 64K 余量）：
`launcher_l 0x3A0000/1M`、`retro-core_l 0x4A0000/1536K`、`gbsp_l 0x620000/1280K`。

## 4. NVS 契约（唯一真源）

| key (namespace `ui`) | 类型 | 含义 |
|---|---|---|
| `orient` | u8 | `0` = 竖屏，`1` = 横屏；**不存在 = 从未选择过**（首次开机） |
| `orient_ver` | u8 | 契约版本号，当前 `1`；将来改语义时用于迁移/丢弃旧值 |

读取点必须**单一**：`rg_system_init()` 之前的一个函数 `rg_orient_get()`（缓存一次），
UI 与切换逻辑都只走它，禁止各处直接 `nvs_get_u8`（§"索引/真源必须唯一"）。

## 5. 启动流程

```
app_main
 ├─ rg_orient_get() → 无记录？
 │    ├─ 是 ⇒ 画「首次选择页」（当前驱动的可见区居中排版，两个大按钮）
 │    │       → 写 orient + 若选了"另一套" ⇒ set_boot_partition + 提示"重启生效" → esp_restart()
 │    └─ 否 ⇒ 正常进 launcher
 └─ 设置菜单里「屏幕方向」项
      → 选择页（居中弹框：会重启 / 请先存档 / [取消] [重启切换]）
      → 点「重启切换」才 set_boot_partition + esp_restart()；点取消回到菜单
```

弹框文案（英文默认，与现有 UI 一致）：
> Switching screen orientation will restart the device.
> Make sure you saved your game before continuing.
> [ Cancel ]  [ Restart & Switch ]

## 6. 实现清单（文件级）

| 文件 | 改动 |
|---|---|
| `components/retro-go/rg_orient.c/.h`（新） | NVS 读写 + `rg_orient_apply_slot()`（封装 `esp_ota_get_running_partition` + `set_boot_partition`）+ 首次开机判定 |
| `components/retro-go/rg_gui.c` | 新增 `rg_gui_dialog_orientation()`：两按钮选择页 + 重启确认弹框（复用现有 `rg_gui_dialog` 风格） |
| `launcher/main/*` | 启动时若"首次未选择"⇒ 出选择页（在画 tab 之前） |
| `retro-core/main/main.c` | 同上（核心也可能首启？默认核心不弹，只有 launcher 弹 —— **待定**，见 §8） |
| `launcher/main/settings`（现有设置菜单） | 加「Screen orientation」项 → 走同一弹框 |
| `targets/tab5/partitions.csv`（或等价） | 增 `ota_0`，`factory` 保持竖屏 |
| `tools/flash-tab5.sh` | `--dual` 支持 |
| `tools/build-tab5-dual.sh`（新） | 依次构建两套 → 组装 dual 刷机参数 |

## 7. 验证计划（每步都要真机证据）

1. 分区表烧好后：`esptool read_flash` 两侧镜像 sha256 与本地一致（断言产物身份，见经验沉淀 §230）。
2. 首次开机（清 NVS：`nvs_flash_erase` 或 `esptool erase_region nvs`）⇒ 选择页出现，两个按钮都能点。
3. 选"横屏" ⇒ 弹框出现 ⇒ 点 Cancel 不重启、回到上一页；点 Restart ⇒ 重启后横屏生效（日志 `lcd_init ... 90CW map`）。
4. 再选回竖屏 ⇒ 重启后日志无 `90CW map`、竖屏布局正常。
5. 断电重开：方向保持（NVS 生效）。
6. 回归：横屏下 GB 4x / 按键布局 / 无 `Bad lcd window` / 无 `invalid addr` 全部保持。

## 8. 待用户拍板的点

1. **首次开机是谁弹**：只 launcher 弹（推荐），还是每个 app 都弹？
2. 切换后是否要做"存档检查"（读 SRAM 存档状态提示"有未保存进度"），还是只给静态文案？（推荐先静态文案）
3. 分区方案确认：`factory`=竖屏 保底、`ota_0`=横屏 —— 或反过来（若用户希望横屏成为主用方向）。
4. 是否需要在"方向"之外**顺带**记录"上次运行的游戏"以便切回后继续（不推荐，超出本 SPEC）。
