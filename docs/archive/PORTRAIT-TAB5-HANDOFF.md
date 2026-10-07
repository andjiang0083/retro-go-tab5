# Tab5 竖屏 retro-go —— 交接文档（2026-09-29 夜 更新）

## 一、当前状态

### 🟢 最新（2026-10-06 第三轮）：v0.4.6 —— 修「用 M5Launcher 装完跑不起来」+ SD 装载错误兜底

**① 用 M5Launcher 装本固件后进不了游戏（旧版本还会崩）** —— issue [#7](https://github.com/andjiang0083/retro-go-tab5/issues/7)
- 根因（代码级）：本固件是**双 app**形态（`launcher` 菜单 + `gbsp` 核心各占一个分区），而"某核心算不算已安装"
  在双 app 形态下是**运行时查分区**（`rg_system_have_app()` → `esp_partition_find_first(APP, ANY, "gbsp")`）；
  **M5Launcher 的安装器只抽取内嵌分区表里的第一个 app**（`src/sd_functions.cpp: updateFromSD()` 读 0x8000 分区表
  → `installFirmwareDynamic(..., appOffset, ...)`）⇒ 整包装进去只有菜单、没有核心分区 ⇒ `applications.c` 里
  每个 `application()` 都早退 ⇒ **零标签页** ⇒ `gui_set_current_tab()` 返回 NULL，而主循环无条件解引用
  `tab->enabled` ⇒ **空指针崩溃** = 用户说的"跑不起来"。
- 真机证据（T2）：刷 M5Launcher 2.9.1 + 用它**自己的安装通道**装**单 app 包** ⇒ `launcher v0.0.1-87-g2d30c` 正常、
  `Storage mounted at /sd`、`Touch ready`、`gui_add_tab: Tab 'gba' added at index 0`、无 panic；安装后真实分区表
  `[4] retrog app/ota_0 off=0x1a0000 size=0x140000`（与我们的 app 分区尺寸一致）⇒ **单 app 包经 Launcher 安装能跑**，
  用户撞的是"装错文件（整包）"。
- 修法：① `launcher/main/main.c` **零标签页防护**（不再崩）；② `applications.c` 在缺核心分区时**弹窗自解释**
  （直接把原因与动作摆屏幕上，而不是留空白界面）。
- ✅ **真机验证（T11，2026-10-06 22:12）**：用 Launcher bootloader+分区表 + **新构建的菜单 app** 复刻"只有菜单"布局 ⇒
  `[error] applications_init: No emulator core partition found: this is a dual-app build…` 弹窗出现在屏幕上，
  崩溃迹象 grep = **0 匹配**（Panic/Guru/LoadProhibited/abort 全无）。
- ⚠️ 上游已知：M5Launcher **2.9.1 的 Tab5 安装路径本身偶发崩** —— 实测传输到约 40KB 处设备 `E BOD: Brownout
  detector was triggered`（欠压复位）；其 changelog 自述 2.10.0 才修 "FIXED M5Stack Tab5 OTA function randomly
  crashing"（尚未发布）⇒ 文档写明"装失败就重试，或用 M5Burner / esptool 刷整包"。

**② SD 提示 "Storage mount failed"（0x107）** —— issue [#8](https://github.com/andjiang0083/retro-go-tab5/issues/8)
- 现象：进固件弹 `SD Card Error / Storage mount failed…`，菜单无游戏；串口 `sdmmc_init_ocr: send_op_cond (1)
  returned 0x107`（ESP_ERR_TIMEOUT）⇒ 卡对 CMD1 完全不应答，降速重试同样失败。
- 定性（**三条真机实验定案**）：卡槽 I/O 供电走 **ESP32-P4 片上 LDO 的 chan4**（厂商 BSP 原注释
  `LDO_VO4 is used as the SDMMC IO power`，`BSP_LDO_PROBE_SD_CHAN=4`/3300mV）；本固件历来**从不申请片上 LDO**
  （历史原因：过早申请会拖死 MIPI DSI PHY 上电），默认状态能读卡 ⇒ 这份硬件状态**会被别的固件（Launcher 自己
  会配 chan3/chan4）改掉且跨软复位保留**，于是"装过 Launcher 回来"就读不到卡。

  | 手段 | 结果 |
  |---|---|
  | 仅"失败后启用片上 LDO chan4 重试" | ❌ 仍 0x107（但证明启用 LDO **不再拖死显示** ✅） |
  | 厂商配方（slot0 + 4-bit + LDO + 显式引脚） | ❌ 仍 0x107 |
  | **重插卡 + 真断电**（不是复位） | ✅ **立即恢复**（`Storage mounted at /sd`，首次尝试即成功、未走回退） |

  ⇒ 属"卡/供电进入不可应答态"，**软件侧救不回来** —— 已记 `~/esp32/ESP32-经验沉淀.md` §183。
- 修法：① 首次挂载失败时按厂商写法启用 LDO chan4 **重试一次**（正常开机路径一字未动；实测无 DSI 副作用）；
  ② 留存 `RG_STORAGE_SDMMC_VENDOR_RECIPE`（默认 0）作可复现对照；③ 文档写明用户侧规避 = **重插卡 + 真断电**。

### 🟢 上一版（2026-10-06 第二轮）：v0.4.5 —— 修两处 GBA 移植缺失（横条闪烁 + 启动器选存档）

**① 画面横条闪烁**（issue [#5](https://github.com/andjiang0083/retro-go-tab5/issues/5)）—— **已修，用户真机目视确认「画面没有横条了」**
- 根因：GBA 主循环只建了 `updates[0]`、`currentUpdate` **永不轮换**；而 `rg_display_submit()` 只是把**指针**交给
  显示任务，显示任务在**另一个线程**异步读这块内存（`rg_display.c: display_task` → `write_update(msg.dataPtr)`）
  ⇒ 核心渲染第 N+1 帧时显示任务可能还在读第 N 帧 ⇒「上半屏旧帧 + 下半屏新帧」= 一条条横条。
  上游 NES/GBC/SNES/gwenesis 都是**两块 surface + 提交后轮换 + 切核心画面指针**。
- 为何快速移动才明显：显示任务每帧工作量 ∝ 变化行数。静止时每帧 ~0.4 个 32 行带（实测 `blocks=18/s`、
  `display=16ms/s` ≈1.5%），核心画完下一帧时显示早就读完；快速卷轴时近整屏 15 带 ≈13ms > 核心画下一帧 ~8–12ms ⇒ **必然重叠**。
  v0.4.4 把帧率 34.7→59.7 后每秒提交次数 ×1.7，把**既有缺陷**放大到肉眼明显。
- 修法（`gbsp/main/main.c`）：建 `updates[1]`；提交后 `currentUpdate = updates[currentUpdate == updates[0]]`；
  `gba_screen_pixels = currentUpdate->data`（核心每帧现读 `get_screen_pixels()`，轮换安全）。建不出来退回单缓冲（仍有横条但不崩）。
- ⚠️ 诚实记录：我另做的逐帧仪表（提交时显示任务是否仍占着上一帧）**不能作为量化证据** —— 抓取窗口里游戏几乎静止
  （`blocks=18/s`），两轮读数都 ≈0；且它采样时机偏早（真实重叠发生在提交**之后**）。要量化「改前/改后」需在**真运动**场景抓取（本轮没做）。
- 遗留（未动）：DPI 只有一个帧缓冲且写入不与面板扫描同步；如需再压可用 IDF 的 `on_refresh_done` 做 vblank 锚定。

**② 启动器「继续游戏 + 存档位」不生效**（issue [#6](https://github.com/andjiang0083/retro-go-tab5/issues/6)）—— **已修，真机验证**
- 根因：gbsp **从未消费 `RG_BOOT_RESUME`** —— 其它 8 个核心（NES/SNES/GBC/SMS/PCE/GW/fmsx/gwenesis）都有
  `if (app->bootFlags & RG_BOOT_RESUME) rg_emu_load_state(app->saveSlot);`，而 `gbsp/main/main.c` 里连 `bootFlags`
  都不出现 ⇒ 启动器给的存档位被丢弃（只能再从 menu 手动读档）；「保存并退出 → 下次开机自动续上」同样失效。
  启动器→核心的管道本身是好的（`applications.c:127-140` 写 flags → `rg_system_switch_app` → `update_boot_config` 落盘）。
- 修法：`sram_load()` 之后、主循环之前补那 3 行（位置与上游一致）。
- 验证（真机日志）：`RG_TEST_RESUME: forced bootFlags=01 slot=0` → `rg_emu_load_state: Loading state from
  '.../恶魔城_晓月之圆舞曲.gba.sav'`，无 `Load failed!`。

**发版 0.4.5**：GitHub Release + M5Burner（见 §六）。

### 🟢 上一版（2026-10-06）：v0.4.4 已打包发布 —— 修好 GBA dynarec 下的"按键不响应"

**根因**：两个执行引擎的**周期记账口径不一致** —— 解释器按 `ws_cyc_nseq/ws_cyc_seq[区域][索引]` 动态扣
等待周期，dynarec 写死常数（访存 load+2/store+1、取指用 `def_seq_cycles`、MUL/MLA 估算 +2/+3）。
后果不是"算错某个值"，而是**同一段重活（LZ77 解压 / 关卡加载）在两引擎里跨的 VBlank 帧数差 1 ⇒
游戏自身计时相位永久错 1 帧 ⇒ 输入/动画窗口整体错位**。这解释了为什么此前所有"时序补丁"都无效 ——
它们都没动到这个口径。

**修法**（dynarec 逐口径对齐解释器，见 `gbsp-libretro/riscv/riscv_emit.h` + `cpu_threaded.c`）：
访存 8/16/32 位改为运行时按实际地址查 `ws_cyc_nseq`；取指 `def_seq_cycles[pc>>24][1]` →
`ws_cyc_nseq[pc>>24][0]`；MUL/MLA/长乘/Thumb-MUL 的 13 处估算扣费归零。

**真机验证**（同一 ROM、同一套脚本按键、同一 185 秒窗口）：
| 构建 | 模拟帧率 | 光标事件 | 光标值序列 |
|---|---|---|---|
| 旧 dynarec | 37.5 | 13（全挤在 2603 帧后） | 1,2,4,5,7,8,9 ❌ |
| **修复版** | **59.7** | 29 | **9,24,39,0,9,24,9** ✅ |
| 解释器（基准） | 34.7 | 21 | 9,24,39,0,39,24,9 ✅ |

用户真机确认：**帧率正常 + 名字输入界面四方向键全部正常**。
**反直觉结论**：扣费更真实 ⇒ 每帧要执行的 guest 指令更少 ⇒ 帧率反而 37.5 → 59.7fps（模拟更准不一定更慢）。

**残余（已定性，不修）**：与解释器差**恒定 1 帧**且速率相同（f=50→400 两者计数器均降 342）——
dynarec 在**块边界**结算周期/中断、解释器按**每条指令**结算，帧边界落点可差一个块（PC `0x0B30` vs `0x0B34`）
⇒ 游戏内一次二选一判定翻转。结构性差异，无功能影响。
**仍未对齐的两个口径**（会"增加" dynarec 扣费，改前先备份并复测）：① LDM/STM 每寄存器 `+1` →
应为 `ws_cyc_seq[区域][1]`；② 直接分支 B/BL/BX 未扣 → 应为 `ws_cyc_nseq[目标区域][1]`。

**发布物料与状态（2026-10-06 完成）**：`dist/m5burner-0.4.4/`（merged 2,293,760 B / sha256 `2bf47ee1…`；
单 app 1,376,256 B / sha256 `83bcd599…` + 双语 README + 字段表 + Release Notes）。
- **GitHub Release v0.4.4 已发布**：<https://github.com/andjiang0083/retro-go-tab5/releases/tag/v0.4.4>
  （8 个附件全部 uploaded，tag → 公开仓 `22ac3c7`；公开仓与 dev 树已同步）
- **M5Burner 已提交**：`retro-go Tab5`（firmwareId `2105333864932450305`）追加版本 **0.4.4**，
  状态 **PENDING / PUBLIC**（待 M5Stack 审核），bin `retro-go-tab5-0.4.4-merged.bin`，
  上传时间 `2026-10-06T19:38:55+08:00`，设备 Tab5。审核通过后需再回读确认 PUBLISHED。
- **发版门禁（本次实测）**：与 v0.4.3 镜像的差异仅三处 —— bootloader 内构建时间戳、
  分区尺寸随 app 自动收缩（gbsp `0x150000`→`0x130000`）、app 段内容；
  bootloader **入口点 `da9ef24f` 完全相同**、分区表偏移量未变 ⇒ 无黑屏风险。
  ⚠️ 注意：这类"分区尺寸自动收缩"是正常的（`build_image` 按 app 实际大小定尺寸），
  不要把它误判成布局变更；真正的判据是 **bootloader 入口点 + 各分区偏移量**。

**测试脚手架（已全部关闭，钩子保留在树里）**：`RG_GBA_DIAG 0`、`RG_GBA_INPUT_TRACE 0`、
`RG_TEST_KEYS_DEVICE ""`、`RG_TEST_NO_AUTOSAVE 0`、`#undef RG_TOUCH_TRACE`。
⚠ 坑：`RG_TOUCH_TRACE` / `RG_TEST_KEYS_DEVICE` 是 **`#ifdef` 判定**，设 0 无效（必须不定义 / 置空字符串）。


### 🟢 最新（2026-09-29 深夜）：v0.4.3 已打包待发布 —— 修好"M5Launcher 装进来玩不了"
- **本轮主题**：用户反馈"从 bmorcelli/Launcher 装这个应用后读不到 SD、无法游玩"。查清是三处断点：
  ① 字库独占一个 flash 分区 → 那种启动器重建数据分区时会丢（中文变方块）；
  ② **本固件是两个 app**（菜单 `launcher` + 核心 `gbsp`），而它只装"第一个 app"（维护者 issue #343
     原话 "always fetch the first app partition"）→ 核心永远装不上 → 这就是"无法游玩"；
  ③ SD 挂载位宽（它 1-bit / 我们 4-bit）**尚未定性**，用户要求稍后处理。
- **① 已修**：中文字库改为**编进 app 镜像**（`target_add_binary_data` + `rg_cjk.c` 直读
  `_binary_cjk12_bin_start`）；顺带总镜像小了 64KB（少了独立字库分区）。
- **② 已修**：新增**单 app 形态**（编译期 `-DRG_SINGLE_APP=1`，构建入口 `rg_tool.py --single-app`）——
  菜单与核心编进同一个 app；切换方式由"重启到另一个分区"改为"NVS 待续标志 + 重启自己"
  （**唯一分歧点**：`rg_system.c` 的 `update_boot_config()`；核心源码一行没改）。
- **真机验证（两种形态都过）**：
  - 单 app 镜像 `retro-go_v0.0.1-82-g532c3_tab5-single.img`（1,507,328 B）：菜单列出 GBA 游戏 ✓、
    进游戏 ✓、中文正常 ✓、存档写入 ✓、退出回菜单（日志 `rg_system_exit` → `Switching to app launcher`
    → 菜单重启）✓、0 报错；
  - 双 app 镜像 `retro-go_v0.0.1-82-g532c3_tab5.img`（2,424,832 B）：分区表 `launcher`(960KB) +
    `gbsp`(1344KB) ✓、`cjk: font ready … 103 KB embedded` ✓、0 报错。
- **本轮两个构建陷阱**（都已记入 `~/esp32/ESP32-经验沉淀.md`）：**§126** 全局编译宏加在
  `rg_setup_compile_options()` 里对 `retro-go` 组件**无效**（它自己写了一套
  `component_compile_options`）→ 症状是"菜单里游戏列表全空"，没有任何编译错误；
  **§127** CMake 缓存变量是"粘"的 → 布尔开关必须每次显式传 0/1，否则双 app 构建会混进单 app 产物。
- **发布物料**：`dist/m5burner-0.4.3/`（两份包：merged 给 M5Burner/esptool、launcher-singleapp 给
  M5Launcher）+ 仓库根新增 `README.md`（第一屏就是"你是从哪儿装 → 装哪一份"）。
- **GitHub 侧已完成**（本轮）：代码已同步到公开仓（`publish-github.sh` → `27965b5`），
  公开仓 README/README_CN 补上"先选对文件"表与 `--single-app` 构建命令（`fbdba20`），
  **Release 已建并回读验证**：<https://github.com/andjiang0083/retro-go-tab5/releases/tag/v0.4.3>
  （附件 merged bin 2,424,832 B / 单 app 包 1,441,152 B / 封面，均 uploaded）。
- ⏳ **仅剩**：用户本人在 M5Burner 里点 Publish（字段照抄 `PUBLISH_FIELDS.md`）；
  以及 ③ 的 SD/Launcher 实机复现。

### 上一轮（v0.4.1/0.4.2 时期，保留备查）
- **v0.4.1 已发布**：M5Burner 0.4.1 由用户本人发布；GitHub Release v0.4.1 含 merged bin / sha256 / 封面 / 说明。
- 本地源码 HEAD = **`558cf51`**（代码改动在 `67ff82a`，`558cf51` 是之后的文档/存证图收尾）：
  v0.4.1 之后的**全项目代码走查处置**，报告见 `docs/archive/CODE-REVIEW-v0.4.1.md`
  （P1×4 全修 + P2×20 处理 19 项，仅"两份显示驱动是否合并"留待决策）。
- 最新构建**并已刷真机**的镜像：`retro-go_v0.0.1-49-g558cf_tab5.img`（2,490,368B，树干净、名字带 HEAD hash）
  —— 回读 launcher/gbsp 段 hash 一致、设备分区表与镜像逐字节一致；运行日志里已出现新代码的
  `tab5_power_init: charging enabled: CHG_QC=on, CHG_EN(P7)=high` 与新的表决日志（= 新固件在跑）。
- 已真机验证：显示占空 35%→7~19%、`-O3`+dynarec、十字键/ABXY 零重叠、
  音量菜单崩溃已修、游戏内存档跨重启、**电量读通（BATT:7637mV ≈ 61%）**、
  **launcher 页眉标题不再被裁**、**电量圆灯已上屏（按键同款配方 + y=1025 空档正中）**、
  **USB-C 充电已修并验证**（电流 -746~-871 mA、电池电压 7627→7745 mV 持续上升）。
- **每帧 DSI/PPA 报错已清零**，但开机后约 5~6s 仍有**一次 ≈0.3s 的
  `lcd.dsi: previous draw operation is not finished` 突发**（本轮 16 条）。已与历史日志逐份比对：
  v0.4 `rel.log` 4.7s / v0.4.1 `x2.log` 9.8s / 本轮 5.8s —— **同一签名、每一轮都在** → **既有现象、非回归**，
  无可见影响，暂不动它（别再当新 bug 查一遍）。
- **充电指示（v0.4.1 起）**：充电中 = 绿**呼吸**（4 档亮度、≈1.4s 一圈）；低电 <10% 仍是红**硬闪**（告警要抓眼）。
  ⚠ 充满后 IP2326 会自动停充 → 电流归零 → 灯不再呼吸（属正常行为，不是坏了）。
- ⏳ **待真机确认（本轮走查改动）**：① 充电灯是否还有偶发单闪（去抖改成"最近 5 次窗口表决"）；
  ② 关机不再被触摸残留点卡住（等键松开加了 3000ms 上限）。
- **方向键改为「矢量扇区判定」**（2026-09-29，**十字外观零变化**；实现 `rg_input.c` 的
  `rg_dpad_keys_at()`）：死区 30px / 轴区 ±30° / 斜区 30°~60° **且** r≥70 / 滞回 5° 与 70→60 /
  可触圆 135。解决两件事：一指能出斜向、从上滑到左不再穿过 86px 空洞断键。
  对照图 `docs/dpad-zones.png`（`python3 tools/preview-touch-overlay.py --dpad`）；
  移植前做过等价性验证：C 整数版 vs PC 浮点版，±150 网格 7442 点 + 11 个采样点**全一致**。
  ⏳ **待用户真机体验**：斜向手感 / 滑动换向 / 阈值要不要调（旋钮就是上面四个数）。
- **新增：X/Y ↔ L/R 调换按钮**（2026-09-29 用户需求："增加一个 lr 和 yx 调换的按钮，
  可以让用户将 lr 切换到更容易点击的地方" —— 格斗游戏里 L/R 要落在拇指够得着的菱形位）：
  L/R 之间那颗琥珀色按钮（`touch_layout.h` 的 `RG_TAB5_SWAP_*`，(360,545) 180×84）：
  键位表 13 个键（含 SELECT/START/MENU）+ 这颗 UI 按钮 = 14 个绘制单元（`btn_count` 仍是 13）。
  点一下 → X↔R、Y↔L 对调（菱形位变 R/L、肩键位变 X/Y，**标签与配色都跟着功能走**），
  再点一下换回；标签显示"菱形位上现在是哪一对"（X/Y ↔ L/R = 状态指示）。
  它不是游戏按键（不注入输入），状态存 NVS（`TouchSwapYX`）断电不丢。
  实现要点：规则只有一处（`rg_touch_overlay.h` 的 `rg_touch_swap_key()`，输入与可视层共用）；
  掩码重建放在渲染线程（`swap_rebuild_if_pending()`），避免输入任务 free 掉 blit 正在读的掩码。
  对照图 `docs/swap-yx-lr.png`（`tools/preview-touch-overlay.py --swap`）。
  **真机五轮踩坑（见经验沉淀 §120~§123）**：
   ① "再按换不回来" → IDF `esp_lcd_touch_get_coordinates()` 无触点时返回 false，
      "抬指复位"写在它的 if 里永远跑不到 → 复位块移到两层 if 外面（`bool any_touch`）。
   ② "点切换后要切界面才生效 / 不是马上变" → 一共四层，缺一层就不动，且**只能由显示线程去做**（§120/§121/§122/§123）：
      - 菜单是**事件驱动重绘**（调换不产生按键 → 根本不重画）→ 启动器循环比 `rg_overlay_get_generation()`，变了立刻重画
      - 显示层按**行校验和**只推变化过的行 → `memset(screen_line_checksum, 0, ...)` 本帧全部重推
      - 显示层推帧**按视口行**遍历，游戏里黑边（y=480..1279）在视口外 → 给"覆盖不到"的矩形
        **重建条带背景**（边框图或纯黑）再走一次发送路径 —— 覆盖层会由驱动在发送前合成上去
      - 消费点：`rg_display.c: write_update()` 里的 `rg_overlay_consume_repaint_request()`
        + `rg_overlay_take_dirty_rects()`（显示线程 ✓）。**顺带修好了游戏里的按压高亮**（同根因）
      - ⚠ **绝不要**在输入任务里调 `rg_display_force_redraw()`：它会 dispatch `RG_EVENT_REDRAW`，
        启动器 event_handler 收到就 `gui_redraw()` → 在输入任务里重画界面 → 与主循环抢 `gui.surface` → **花屏**（§122）
      实现上还要"两套掩码开机预渲染、点击只对换"（不分配不释放 → 跨线程安全且不等下一帧）。
      体检日志（开机一行 + 每次点击两行；条带重建那行是 DEBUG 级，默认不打）：`swap variants built: 4/4 key alts, switch btn alt=1` /
      `touch overlay: X/Y <-> L/R swapped = N (gen M, repaint requested)` / `launcher: overlay gen M -> redraw`。
  ⏳ **待用户真机确认**：菜单里 / 游戏里点一下，标签与配色是否**当场**就变。
- 开机时 `ST7123` 那 5 组 I2C 报错（110~250ms）是**既有现象**（v34 基线逐行一致），不是回归。

## 二、开工必备
```bash
cd ~/esp32/retro-go-tab5
. tools/idf-env.sh
cd retro-go-p4 && python3 rg_tool.py --target tab5 --no-networking build-img launcher gbsp
# 刷机：必须【按提交号点名】挑镜像 —— 脚本现在**没有参数就直接报错退出**（ls -t 兜底已删）
# ⚠ 镜像名里只有 5 位 hash，而 %h 是 7 位 —— 用 7 位 grep 会匹配为空，
#   以前再配上脚本的 ${1:-$(ls -t)} 就会静默退化成"按时间取最新"。两头都要锚住：
SHA5=$(cd .. && git log -1 --format='%h' | cut -c1-5)
IMG=$(ls *_tab5.img | grep -F -- "-g${SHA5}_" | head -1)
[ -n "$IMG" ] || { echo "点名失败"; exit 1; }          # 空就必须报错退出
cd .. && sh tools/flash-retro-go.sh "$IMG"             # 脚本：无参数报错、文件不存在报错、写前比对分区表
sh tools/log-tab5-nr.sh 60 > /tmp/x.log                # 抓日志用不复位版
# 看效果（不刷机）：按键/圆灯预览 → docs/led-variants.png；发布封面 → dist/m5burner-<ver>/cover-320x200.png
python3 tools/preview-touch-overlay.py --led
python3 tools/make-cover.py 0.4
python3 tools/make-cover.py --hero docs/screenshot-portrait.png
```
设备端口 `/dev/cu.usbmodem*`（自动探测；插拔后会变号）。SD 卡 `/Volumes/SD/`：**只读，不写不动**。

## 三、纪律（都是踩过坑换来的）
1. **一次只改一个变量**；实验被否**整笔回退**到已验证基线，**不在失败点上叠加第二刀**。
2. **出错/被否必记** → 追加 `~/esp32/ESP32-经验沉淀.md`（§N 格式，现已到 §117）。
3. **遇到别人做过的功能，先读参考实现的注释**，别自造插桩试参数（§110）。
4. **不猜**：能抓 backtrace 就抓（USB 通时）；崩一次胜过十轮猜。
5. **视觉改动先在 PC 重渲染给用户看**（`--led` / `make-cover.py`），确认几何/配色后再刷机 ——
   本次圆灯就是"先出三档位置对比图 → 用户点 3 → 才改固件"，一次成型，没浪费刷机轮次。
6. 用户会看报告末尾的「待办建议」并据此批准执行 —— 待办必须写具体（文件/参数/验证方式）。

## 四、待办

### ⏸ 挂起（2026-10-07）：GBA 内核（gbsp）差距评估 —— 已分析固化，**等用户量再推进**

**一句话**：客户反馈《宝可梦弹珠台》"不支持" → 三层取证 + 与来源镜像全量 diff ⇒
**我们与 `Irak4t0n/HowBoyAdvance`（我们的 RISC-V 移植来源）只差 20 个文件的零星差异，渲染器已是新版**
（含上游 2023-08 的 OBJ 透明修复）；值得摘的是 mosaic 计数器一处真 bug + 存档检测等增量。
（⚠ 当天我一度判成"渲染器落后两代"，被全量 diff 推翻 —— 错因是 grep 漏了 `.cpp` 扩展名，已留档。）

- 完整评估（被推翻的假设、逐文件差异表、摘取优先级、外部 issue 索引）：**`docs/GBA-CORE-UPGRADE-ASSESSMENT.md`**
- **用户决定（2026-10-07）：不急着推动** —— 等假期结束、M5Burner 审核通过、更多用户先接触到新版 v0.4.7
- ⚠ **客户问题根因仍未定**：需「具体现象（花屏/重启/冻住/报错）+ 固件版本 + 复现率」；
  零成本第一刀 = 宿主（解释器）逐帧 PNG，第二刀 = 真机 dynarec vs 解释器 A/B
- 第一步（具体）：拿到现象后按评估文档 §六 执行；只有确认要同步时，才按 §五 的 P1→P4 顺序摘 hunk
- **硬约束**：① 不追上游主仓（无 `riscv/`）② 跟 HowBoyAdvance **按 hunk 摘取，不整仓也不整文件替换**
  （对方 `gba_memory.c` 的 RFILE/mirror/mini-ROM 模型与我们的分块+缺页回读冲突）③ 必须保住 v0.4.4 的
  dynarec 周期记账口径对齐（对方与上游命中数均为 0）+ 我们的 `gba_over.h`（比对方多 784 行差异，我们更全）
- 同一批挂起项：GBA zip 的假承诺（撤声明 + 清 14 处文案 + 启动前格式门禁，未实施）、
  失败回执（ROM 体检一行 + panic 落 SD 日志）

### ⭐ 本轮最新（2026-10-07）：SNES 帧率攻坚 —— 已固化，**暂停待续**（非发布内容）

**一句话：10fps → 57.8fps，瓶颈已量化到一行代码，最后 518µs 未攻。**

- 完整记录：**`docs/archive/SNES-PERF-SESSION-2026-10-07.md`**（起点/终点、测量体系、因果链、未走的路、恢复方法）
- 工作固化：分支 `perf-probe/snes-2026-10-07`（`27df21e`）+ 仓库外备份 `~/esp32/snes-perf-backup/snes-perf-probe-2026-10-07.patch`
- **设备已刷回发行版 v0.4.7**（`dist/m5burner-0.4.7/retro-go-tab5-0.4.7-merged.bin`），探针日志命中 0 次

| 项 | 内容 |
|---|---|
| 已确认的瓶颈 | `audio_task` 一轮 18.5ms = 等 I2S 16.7ms（必然，驱动注释 "the I2S DMA paces the emulator loop"）+ **DSP 混音 1.8ms（净多出来的）** ⇒ 周期 > 帧时间 ⇒ 队列积压 ⇒ 主循环 `send` 干等 5.2ms ⇒ 钉在 57.8fps。**同一根因造成声音发飘** |
| 顺手修到的上游 bug | `rg_task_create_ex` 接收 `queueLen` 却从未写入结构体 ⇒ 所有任务队列深度恒为 1。**建议提 PR 给上游**（与本次调优无关，是独立缺陷） |
| 下次接着干 | ① 优化 `S9xMixSamples`（定点化/内联化，音质无损、工作量大）② 降采样率 32000→22050（省约 560µs，**数字上刚好补上缺口**，代价是高音有损） |
| 别再试 | PPA 硬件缩放（六轮 `err=0x102`）、换 snes9x2005/2010 内核（更精确=更慢）、65c816 dynarec（CPU 仅占核心 20%，收益约 5%） |
| 方法论教训 | 屏幕探针是**瞬时值**，不能用来判效果 —— 本轮据此误判过一次。判改动一律 "串口抓 ≥200 组 → 算均值/σ → 再下结论" |

### ⭐ 上一轮（2026-10-06 第二轮）：v0.4.5 —— 两处移植缺失已修并验证

| # | 问题 | 根因 | 状态 |
|---|---|---|---|
| ① | 快速移动时**横条闪烁**（issue [#5](https://github.com/andjiang0083/retro-go-tab5/issues/5)） | GBA 主循环只有一块画面缓冲且**永不轮换**（`updates[0]`），显示任务在另一线程异步读它 ⇒ 核心渲染下一帧时显示任务还在读上一帧 ⇒「上旧下新」拼图 | **已修**（建 `updates[1]` + 提交后轮换 + 切 `gba_screen_pixels`）；**用户真机目视确认横条消失** |
| ② | 启动器**「继续游戏 + 存档位」不生效**（issue [#6](https://github.com/andjiang0083/retro-go-tab5/issues/6)） | gbsp **从未消费 `RG_BOOT_RESUME`**（其它 8 个核心都有那 3 行）⇒ 存档位被丢弃 | **已修**（`sram_load()` 后补 3 行）；**真机日志验证**（`forced bootFlags=01 slot=0` → `Loading state from ...gba.sav`，无 `Load failed!`） |

- 两个 issue 正文都是**中英双语**，含根因、代码位置、验证证据、以及诚实标注的"仪表不能当证据"。
- 发版：**0.4.5** → GitHub Release + M5Burner（物料 `dist/m5burner-0.4.5/`）。
- 待办（可选，未做）：横条「改前/改后」的**真运动**量化；DPI 面板侧 vblank 锚定（`on_refresh_done`）。

### ⭐ 上一轮（2026-10-06）：v0.4.4 已发行；残余问题挂到 issue #4

**发行状态（均已回读验证）**
- **GitHub Release v0.4.4 ✅** <https://github.com/andjiang0083/retro-go-tab5/releases/tag/v0.4.4>
  （8 个附件全 uploaded；tag → 公开仓 `22ac3c7`；issue #3 已结案关闭）
- **M5Burner ✅ 已提交**：`retro-go Tab5`（firmwareId `2105333864932450305`）追加 **0.4.4**，
  状态 `PENDING / PUBLIC`（待 M5Stack 审核，**用户无需操作**），bin `retro-go-tab5-0.4.4-merged.bin`，
  上传时间 `2026-10-06T19:38:55+08:00`；旧版 0.4.3 仍 `PUBLISHED` 在线。
  ⇒ **后续动作：某天用登录态接口回读一次，确认 0.4.4 → `PUBLISHED`。**

**① 残余问题 → issue [#4](https://github.com/andjiang0083/retro-go-tab5/issues/4)：P0-a 已做完，原假设被否**

现象：dynarec 与解释器在 BIOS LZ77 循环的**帧边界落点**差 1 帧（块首 PC `0x0B30` vs 块内 `0x0B34`）。

**P0-a（2026-10-06 已做完，两轮真机各 330s）**：采样点从「宿主帧边界」搬到**模拟 VBlank 起点**
（核心扫描线 `vcount==160` 分支，两引擎共用那段代码），读取方式为直接读 EWRAM 页表指针（纯内存读、
零副作用）；同时把按键脚本时钟从**真实秒**改成**模拟帧**（`frame_counter/60`）。

| 观察 | 实测 | 结论 |
|---|---|---|
| 引导/加载窗口（f=0..300）锚点 vs 宿主边界 | **都是 Δ=-1、98%** 吻合 | **采样时刻不是伪差来源**（原假设被否） |
| 同窗口逐帧原始值 | dyn `02F0 02EF…` ｜ itp `02EF 02EE…` | dynarec **恒定领先 1 帧**（真实、稳定） |
| 加载之后全程 `dir`（输入状态机） | 无位移能给高吻合率（最佳 Δ=-2 仅 60%） | 不是恒定相位差，而是**相对漂移** |
| 游戏可见状态（光标） | **任何位移都 99.9% 吻合** | 残余**不影响可观察结果** |
| 按键脚本对照 | 原按**真实秒**注入（两引擎帧率差 1.7×） | **这部分确实是我们自己造的**（已改帧时钟） |

**下一步（替换原计划）**
1. **P1（建议做，约 25 分钟设备时间）**：在**完全不改上游**的树上、用同一套夹具（帧时钟 + guest 锚点）
   跑同一 ROM ⇒ 看 Δ=-1 是否本来就有。若基线也有 ⇒ 与 v0.4.4 **无关**（既有实现差异）。
2. **P2**：v0.4.4 那两处"未对齐口径"分别单独加上，看 1 帧领先是否移动 ⇒ 定位由哪个口径造成。
3. **P3**：加载后的"漂移"是否要处理 —— 前提是能证明它影响可观测行为（目前证据是"不影响"）。
4. **P4（最后）**：只有 1、2 都指向"真实且有影响"才动扣费，必须同窗口复测帧率 + 输入。

⚠️ **本轮数据不能用来测帧率**（为逐帧取证把日志打满 ~2MB/轮，UART 把两轮都拖到 ~57fps）；
帧率仍以发行版实测 **59.7fps** 为准。DIAG_FB 在 Δ=0 时只有 64% 吻合（同为宿主帧边界采样 ⇒ 需重取，暂不作证据）。

**② 性能（用户定调：以此状态为基准；无止境，但不阻塞任何事）**

基准 = **59.7 fps**（dynarec，185s 窗口；解释器 34.7）。可选只有两项：`-Oz` 回 `-O3` 复测、
P4 两条口径复测。

### 0) ✅ v0.4.3：M5Launcher 兼容（已真机验证 + **已上架** M5Burner）
字库内嵌 + 单 app 形态 + 两份发布物料；只差用户本人发布。
### 0.5) ✅ ③ SD 读不到 —— 已定性并收口（2026-10-06 第三轮，issue #8）
**旧候选修法（4-bit 失败回退 1-bit）与真因无关，未采用。** 三条真机实验定案：真因是**卡槽状态被别的固件改坏
且跨软复位保留**（卡槽 I/O 走 P4 片上 LDO chan4，本固件历来不申请），属"卡进入不可应答态"——
启用 LDO 重试 ❌、厂商配方 slot0+4-bit+LDO ❌、**重插卡 + 真断电 ✅ 立即恢复**。
固件侧已加"首次挂载失败时启用 LDO chan4 重试一次"作兜底；用户侧规避写进 README（重插卡 + 真断电）。详见 §一 v0.4.6 ②。
### 1) ✅ 充电中绿闪 —— 已修 + 真机验证（v40 `gef349`，2026-09-29）
- **根因**：`bsp_io_expander_pi4ioe_init()` 往 PI4IOE2 输出寄存器写的是 `0b00001001`（只置 P0/WLAN_PWR_EN、
  P3/USB5V_EN），**P7(CHG_EN) 是 0** —— 它上面那行注释和被注释掉的 `0b10001001` 才是带 P7 的。
  于是充电芯片 IP2326 一直禁用，插 USB 也不充（旧记录里"USB 供电但电流≈0"就是这个原因，不是"不需要充"）。
  官方 demo 在同一个位置**显式补了一次**：`setChargeQcEnable(true); delay(50); setChargeEnable(true);`
  （`M5Tab5-UserDemo/platforms/tab5/main/hal/hal_esp32.cpp:61`）。
- **修法**：`drivers/display/mipi_dsi_tab5_p.h` 在 `bsp_io_expander_pi4ioe_init()` 之后照官方补那三行（提交 `ef34928`）。
- **真机证据**：`charging enabled: CHG_QC=on, CHG_EN(P7)=high`；`INA226-CHG: shunt=-1742 (-871 mA) -> charging=1`；
  电池电压 7627 → 7735 → 7745 mV 持续上升 → 圆灯切到**绿闪**。
- 判定口径不变：`rg_input.c` 读 INA226 分流寄存器 `0x01`（2.5µV/LSB，5mΩ → `mA = raw×0.5`），
  **负值 = 充电**，阈值 `RG_TAB5_CHARGE_CURRENT_MA=40`。
- **遗留观察（未做）**：分流读值会抖（-871mA 与 +1mA 交替采样），单次采样让 `charging` 在 2 秒周期里翻一次
  → 视觉上是"闪 2s / 停 2s"。要更稳就改成"连续两次同向才改状态"（一个变量，等用户决定）。

### 2) ✅ 发布 v0.4.2 —— 已被 0.4.3 / 0.4.4 取代（历史记录；两次都已实际发布上架）
- **物料已备好**：`dist/m5burner-0.4.2/`（merged bin 2,490,368 B + sha256 + 320×200 封面 + 双语
  `README_M5Burner.md` / `GITHUB_RELEASE_NOTES.md` / `PUBLISH_DESCRIPTION.txt` / `PUBLISH_CHANGELOG.txt`
  + 两张配图；bin 不入库）。逐字段抄 `PUBLISH_FIELDS.md`：Name `retro-go Tab5`、Version **`0.4.2`**、
  类别 `tab5`、author `andjiang`。
- 镜像校验已过：bootloader@0x2000 `e903024f`（DIO+ESP32-P4）、partitions@0x8000 `aa50`、
  sha256 `89a9898c…`（含调换键修复的最终版，固件戳 `v0.0.1-69-g08046`）、
  **前 64 KB 与 0.4.1 逐字节一致**（bootloader/分区表未动）。
- 0.4.2 内容：**调换键** + **调换键默认态功能反了（按 X 出 R，§124）** + 游戏内按压反馈/列表花屏/
  抬指复位三处修复 + 方向键矢量扇区 + 刷新优化。
- GitHub 发布：本机 `git remote` 为空（公开仓不在此目录）→ 推送 / 发 Release 需用户确认后再做。

### 3) 跳帧已可关到 0（2026-09-29，已完成并真机验证）
- 根因：自动跳帧两处下限都是 1（`app.frameskip = 1` 初始值 + `app.frameskip > 1` 才降档），
  而核心是 `skip_next_frame = app->frameskip` ⇒ **渲染永远封顶 30fps，与性能无关**。
- 实测（钉死 0）：skipped=0、速度 100%、BUSY 62~69%、显示 216ms/s（xpose 180 + ovl 12 + draw 22），
  0 丢块 ⇒ 60fps 满帧有 ~30% 余量。
- 改动（`rg_system.c` 自动跳帧策略）：去掉 >1 下限；降到 0 额外要求 busy<75%（75~85% 死区做滞回）；
  升档后 20 秒内不再降回 0。验证：游戏里默认落在 0，重场景才升档，改档间隔 15~30s，无横跳。
- **PPA 第三次也是最后一次量：整帧一次 16.8ms/帧（82MB/s）、按块 1129µs/块，都比 CPU 路径慢一倍
  ⇒ 显示路径定案走 CPU 直写，不再回头折腾。** 详细见 §125。
- 坑：抓日志进程占住串口 → 刷机报 "Packet content transfer stopped"；刷前先 `pkill -f log-tab5` + `lsof` 确认。

### 4) 遗留小项
- 浏览列表时日志有 `Texbox (pos: 680x41, size: 264x0)` 截断告警：v0.0.1-16 就有 → **既有问题**，未动。
- `exp/display` 与 `portrait` 两条分支并存；`exp/display` 是 `portrait` 的祖先（0.3 的内容都在 0.4 里）。
- v0.4.1 走查 10 项只剩【P2-14 可选】**触摸去抖加固**（窗口多数表决/中值滤波）未做 —— 用户对手感满意，
  按"记档不做"处理；哪天觉得偶发误触再上。
- 开机建叠加层 **328 ms**（其中约 99 ms 是"调换用备用掩码"预渲染，5 个单元）：可用后台任务摊掉，
  但会引入"调换早于构建完成"的竞态守卫，收益 ~100 ms/开机，暂不做。

## 五、已知坑（别再踩）
- **同一条 I2C 上别装第二套驱动**：老 API 会占住 `I2C_NUM_0` → BSP 总线建不起来 → IO 扩展器失败 →
  面板/触摸停在复位（黑屏）。INA226 与 ST7123 触摸屏、扩展器同挂 BSP 主 I2C（SDA31/SCL32）。见 §111。
- **电量按 2S 电包换算**（电包 mV/2 → 3300~4150），别套单节 3.3~4.2V。见 §112。
- **按提交号点名刷机**：镜像名是 `-g`+**5 位** hash；点名失败必须报错，不许退化成 `ls -t`。见 §113。
- **带状态的映射（调换键）必须走 `rg_overlay_map_key()`**：规则自反 ≠ 可以无条件应用；
  输入层与可视层各写一份曾导致"按 X 亮 R"。见 §124。
- **控制区的动态元素**（圆灯这类）必须**直写 DPI 帧缓冲 + 局部 `esp_cache_msync`**：
  驱动只推游戏区（y<480）的条带；且 msync 要求偏移/长度 128B 对齐 —— 一行 1440B ⇒ **行号取 4 的倍数**
  （圆灯条带 `[1012,1040)`：1012×720×2 % 128 == 0 ✓）。见 §114。
- 帧缓冲是 **cached PSRAM**：CPU 局部写必须"先失效该区 → 写 → C2M 写回"，且擦要擦干净（不擦会新旧叠加）。
- **双缓冲**（`35e481c`）→ 菜单往返卡死；**调色板烘 swap**（`efdbaaa`）→ 切画面花屏；
  **驱动侧小推送合并**（`5ff7048`）→ 越界 memcpy 冲垮内存随机崩 —— 三个都已整笔回退。
  教训：凡"合并/攒批"必须钳制范围。
- PPA 可用，但必须**自管 64B 对齐缓冲 + 显式传 buffer_size + 整帧单次 BLOCKING**。
- SRAM 电池存档已修（算路径→开机只读回→运行中检测变化写回；**开机阶段绝不碰 SD**）。

## 六、镜像、包与 GitHub（本次新增的工作流）
- **当前镜像（v0.4.6，2026-10-06 第三轮）**：
  - 双 app `retro-go_v0.0.1-88-g85825_tab5.img`（2,293,760 B）/ 单 app `…-88-g85825_tab5-single.img`（1,376,256 B）；
  - 发版物料 `dist/m5burner-0.4.6/`：`retro-go-tab5-0.4.6-merged.bin`
    （sha256 `7ff5b9ecdd17038785e2a859cd30daca2bc8fcdc883dd21b68f64d0f130daeff`）+
    `retro-go-tab5-0.4.6-launcher-singleapp.bin`
    （sha256 `e78ce332c96a836dce63e4a4773c0b67b21eec6bf9168610cbb8ce324d217b25`）+ `.sha256` ×2 + 封面/配图 + 文案；
  - **已发布**：GitHub Release [v0.4.6](https://github.com/andjiang0083/retro-go-tab5/releases/tag/v0.4.6)（8 个附件，回读 `tag=v0.4.6 draft=false 附件数=8`）；
    M5Burner `retro-go Tab5` **0.4.6 = PENDING / PUBLIC**（22:17:53，vid `2107475429264072706`；
    条目总数 39→40 = 只新增一条，无重复提交）；0.4.4/0.4.5 仍 PENDING，0.4.3 仍 PUBLISHED。
  - **发布门禁（本轮口径）**：`check_merged.py` ⇒ VALID（bootloader@0x2000 / 分区表@0x8000 / app 描述符@0x10020）；
    与 0.4.5 比**镜像头**（不是逐字节）：bootloader `0x4ff29eda`/3 段、launcher `0x4ff00484`/7 段、
    gbsp `0x4ff00408`/7 段、spi 均 `(2,79)`，分区表切片 SHA 逐字节一致；
    bootloader 原始字节仅差 37 B，落在编译时间串（`0x55..0x5b`）与镜像校验和（`0x583f..0x585f`）⇒ 良性。
    ⚠️ 别在 `0x0` 解析镜像头（P4 会得到 `0xff`，真实头在 `0x2000`）。
- **历史（v0.4.5，2026-10-06 第二轮）**：双 app `…-87-g2d30c_tab5.img` / 单 app `…-87-g2d30c_tab5-single.img`；
  物料 `dist/m5burner-0.4.5/`（merged sha256 `f8bd0252…`、单 app `6efcdf38…`）；
  - 双 app（M5Burner / esptool 主产物）：`retro-go-p4/retro-go_v0.0.1-86-gb799a_tab5.img`（2,293,760 B；sha256 `f8bd0252…`）
  - 单 app（M5Launcher 专用）：`retro-go-p4/retro-go_v0.0.1-87-g2d30c_tab5-single.img`（1,376,256 B；sha256 `6efcdf38…`）
  - M5Burner 包：`dist/m5burner-0.4.5/` = merged + 单 app（**均不入库**）+ `.sha256`×2 + 封面/配图 + 5 份文本物料
  - **发布门禁（本次实测）**：三个镜像头（bootloader@0x2000 / launcher@0x10000 / gbsp@0x100000）的
    magic·段数·spi(02,4f)·**入口点**与 0.4.4 **完全一致**（bootloader `4ff29eda` / launcher `4ff00484` / gbsp `4ff00408`）；
    分区表 0x8000 逐字节一致；4KB 块级差异只落在 bootloader 时间戳 + 两个 app 区 ⇒ **无黑屏风险**。
    （方法：`check_merged.py` 全量扫描 108/108 VALID + 逐段头比对；别用单一硬编码偏移。）
  - **已发布**：GitHub Release [v0.4.5](https://github.com/andjiang0083/retro-go-tab5/releases/tag/v0.4.5)（8 个附件，回读确认）；
    M5Burner 条目 `retro-go Tab5`（firmwareId `2105333864932450305`）**0.4.5 = PENDING / PUBLIC**
    （`retro-go-tab5-0.4.5-merged.bin`，2026-10-06T21:02:45+08:00，**仅一条**）；0.4.4 亦 PENDING、0.4.3 仍 PUBLISHED。
  - **发布后自验**：把**上架用的那份 merged**（先 `shasum -c` 校验）整片刷 0x0 进设备，抓启动日志确认可启动
    （见本轮 `/tmp/rel045_boot.log`）。⚠️ 整片刷会抹掉 NVS ⇒ boot config 复位（设备开机进 launcher，属预期）。
- **封面纪律（2026-10-06 22:15 补记，另一个会话修）**：本版包里的封面**曾印过期版本号** ——
  `dist/m5burner-0.4.5/cover-320x200.png` 印 `v0.4.2`、`cover-1280x720.png` 印 `v0.4.3`，
  而包是 0.4.5。已修：`retro-go-tab5-public/tools/make-cover.py` 的版本号绘制改为固定卖点
  （`ROM 放 SD 卡即玩`，**封面永不印版本号**，否则每发一版都要重出图且旧图必与线上矛盾）；
  两个 dist 的封面已换成无版本号版，平台待审 0.4.5 的封面也已换成新图
  （回读 coverId `2107473693807878145`，待审版本改封面立即生效、不走审核）。
  ⚠️ 仍待办：GitHub Release v0.4.5 的 `cover-320x200.png` 附件还是印 v0.4.2 的旧图（替换需 token，未做）。
- 历史（v0.4.4，2026-10-06）：双 app `…-85-g1648f_tab5.img`（2,293,760 B）/ 单 app `…-85-g1648f_tab5-single.img`（1,376,256 B）
- **历史基线（v0.4.3，2026-09-29）**：
  - 双 app：`retro-go-p4/retro-go_v0.0.1-82-g532c3_tab5.img`（2,424,832 B）
  - 单 app：`retro-go-p4/retro-go_v0.0.1-82-g532c3_tab5-single.img`（1,507,328 B）
  - 历史可用基线：`…-34-g074f4_tab5.img`（电量未通但显示/触摸干净）、`…-49-g558cf_tab5.img`
- **M5Burner 包**：`dist/m5burner-0.4.3/` = merged bin（**不入库**，`dist/` 已 gitignore）+ `.sha256`（入库）
  + 封面 + `README_M5Burner.md` / `PUBLISH_FIELDS.md` / `PUBLISH_DESCRIPTION.txt` / `PUBLISH_CHANGELOG.txt` /
  `GITHUB_RELEASE_NOTES.md`；另含 `retro-go-tab5-0.4.3-launcher-singleapp.bin`（M5Launcher 专用，同样不入库）。
  包规范沿用 0.3（merged：bootloader@0x2000 / partitions@0x8000 / launcher@0x10000 / gbsp@0x100000，DIO）。
  ⚠ **单 app 包分区表不同**（只有一个 app 分区，1408KB）→ 必须整包刷，不能只写 launcher 分区。
- **GitHub**：开发树**没有远程**；公开仓 = `~/esp32/retro-go-tab5-public` →
  `github.com/andjiang0083/retro-go-tab5`（分支 `main`）。
  - 同步代码：`sh tools/publish-github.sh "提交信息"`（rsync 开发树 → 公开仓，自动排除
    `dist/`、`*.img`、`backup/` 与 README/LICENSE/.github 等"只在公开仓"的文件，然后 commit + push）
  - 发版：`gh release create`，附件传 merged bin / 封面 / README
  - 凭据：`osxkeychain` 里存着 github.com / andjiang0083（`git push` 直接可用）；
    `gh` 未登录 → `GH_TOKEN="$(security find-internet-password -s github.com -w)" gh ...` 临时注入，**绝不打印**
  - README（公开仓自己维护，不随 rsync 覆盖）：已改成竖屏说明 + `docs/screenshot-portrait.png`；
    **0.4.3 起补上"哪个渠道装哪一份"（M5Burner vs M5Launcher）**——这是对外最容易装错的一处
  - 已发布：<https://github.com/andjiang0083/retro-go-tab5/releases/tag/v0.4>；
    **v0.4.3 已于本轮发布**（回读验证过：附件 merged bin 2,424,832 B / 单 app 包 1,441,152 B / 封面，
    均 uploaded）：<https://github.com/andjiang0083/retro-go-tab5/releases/tag/v0.4.3>


---

## §七 触摸皮肤（0.4.7 待发，2026-10-07 凌晨）

**唯一真源**：`retro-go-p4/components/retro-go/rg_touch_skin.c/.h`
四套皮肤（A 深灰拟物+琥珀 / B 极简线框 / C 琥珀复古 / D 主机配色点缀＝**默认**）
+ 每键 14 槽配色 + 12 个机型条目（铭牌文案 + 点缀色）+ 面板几何（凹槽/四分区 pad/铭牌 pad/字距）。
**PC 预览器 `tools/preview-skin.py` 解析同一文件**（不再手抄常量）。

**接线**：
- `rg_touch_overlay.c`：`rg_overlay_set_skin/get_skin/refresh_panel`、`skin=Console` 进建层日志；
  换皮肤只重算 `pal/pal_p`（**不重建掩码**）+ 原地重画面板 + 控制区整片置脏。
- `rg_display.c`：`rg_display_set_border_surface()` 把内存面板接进**现成的 border 通路**
  （`border_foreign` 标记：外部 surface 只脱手不 free；用户手选 Border 图优先）；
  `rg_display_border_refresh()` 整屏重铺。
- `rg_gui.c`：启动器与游戏两处 Options 都有 `Touch skin`（紧挨 Touch opacity），
  子菜单**光标实时预览** + A 确认 / B 回退；Border 设置改动后回调 `rg_overlay_refresh_panel()`。

**关键坑（真机两轮才纠对）**：`rg_display_get_width/height` 是**逻辑视口**（Tab5 上 720x480），
而面板是**物理全屏** 720x1280 —— 别用"尺寸必须相等"去校验它（我第一版在两处都这么干了：
`rg_overlay_try_panel` 的竖屏守卫 + `rg_display_set_border_surface` 的相等校验，结果面板被静默拒掉，
日志里那句 `应为 720x480` 我当时还误判成"还没转屏"）。正解：
- 该不该装 → 判语义：`console_id != "launcher"`（`rg_overlay_try_panel()`，幂等，挂在 take_dirty_rects/blit 入口）
- 尺寸校验 → 改成"**必须盖住逻辑视口**"（≥ 而非 ==）
顺带收益：launcher 阶段不白花 157ms 生成 + 1.8MB PSRAM（建层 438ms → 229ms）。
装上的判据日志（唯一）：`touch skin: panel installed for 'gba'`。

**控制区怎么被刷出来（Tab5 特有，别再踩）**：逻辑视口==逻辑屏 → `display.changed` 的整屏重铺
条件 `视口<屏` 不成立，**那条路在 Tab5 上什么都不做**；控制区唯一的重铺通道是 `write_update()`
里的覆盖层脏区段。所以面板/边框一变就要给它一条脏矩形 —— 已实现为 `dirty_panel_strip`
（`rg_overlay_refresh_panel()` 置位，`take_dirty_rects()` 返回"整条控制区"一个矩形）。
症状对照：面板装上、日志全对、屏上却纯黑 → 就是这个标志没置。代价 231ms/次（切皮肤时）。

**铭牌/圆灯的排布与灯位真源**：把「上三排底边 ~ 系统键行顶边」这段竖向空间**对半分** ——
上半区居中放铭牌、下半区居中放圆灯（GBA 实测：铭牌 y=965、圆灯 y=1085，间距 ~120px；
旧规则是灯居中、铭牌挤在灯上方，只隔 ~65px，用户反馈"太挤"）。灯位由
`rg_touch_skin_led_cy(ctrl_top, led_r)` 推导，**面板/覆盖层/灯条带三处共用同一个值**，
不要再往任何地方复制 1025 这种常量。

**电量灯的条带背景**：`tab5_batt_led_refresh()` 原来把灯条带擦成**纯黑**（当年代码注释就写死
"那块没别的内容，纯黑"），面板底一旦不是黑就割裂画面。现在擦除色取
`rg_batt_led_band_bg()` → `rg_touch_skin_panel_bg565(skin_idx)`（面板没装时退回纯黑，
与启动器黑底一致）。条带 Y 范围也跟着灯位走，并保持 4 行对齐（帧缓冲一行 1440B，
128B 边界每 4 行一次，msync 要求偏移/长度都对齐）。

**rg_gui 对话框约定（菜单踩全了，改前必读）**：
① NORMAL 选项的回调对 `RG_DIALOG_ENTER` 必须返回 `RG_DIALOG_SELECT`，否则"能选不能确定"；
② `rg_gui_dialog()` 返回 `options[sel].arg`（不是索引），取消返回 `RG_DIALOG_CANCELLED`；
③ option->value 别给 NULL；
④ 预览分"试穿/落定"两条路（`rg_overlay_preview_skin` 不写 NVS / `rg_overlay_set_skin` 写），
   且**写 NVS 要在 early-return 之前**，否则确认时"已是这套"会被跳过、表现成选了不生效。

**验证工具**：
- 回归门禁 `python3 tools/preview-skin.py --check`（与 `docs/archive/skin-candidates/approved-2026-10-06/`
  逐像素比 + **归因**：差异必须全落在面板几何边缘带 ±2px 内，跑出带外即失败）
- 刷机+抓日志一条龙 `sh tools/flash-tab5.sh <merged.bin> [秒数]`（串口独占，BOD 复位当重启重试）
- 构建双形态 `sh tools/build-tab5-skin.sh [single-app]`

**待办**：
1. 进游戏目视四套皮肤 + 菜单切换（需人工点开一个 GBA 游戏；日志判据：
   `panel rebuilt for skin D / console 'gba'` → `内存面板接管边框底图 (720x1280)` → 切换时 `touch skin: switched to ...`）
2. 多核（0.4.8）落地时：把 `rg_overlay_try_panel()` 里写死的 `0, RG_SCREEN_WIDTH, RG_OVERLAY_CTRL_TOP`
   换成从 `rg_display_get_info()->viewport` 取的真实视口 —— 凹槽整形与"上三排下移"会自动跟着走。
3. 发版走 `publish.yaml` + `check_merged.py`；版本号 0.4.7。

## 多核底座（T1–T3，2026-10-07）

Tab5 的缩放是**固定 ZOOM 3x**（`targets/tab5/config.h: RG_DISPLAY_DEFAULT_SCALING = ZOOM`），
视口在**逻辑屏 720x480 内居中**（`rg_display.c: update_viewport_scaling()` 算 left/top = (720-w)/2、
(480-h)/2）。由此定出各机型的窗口与控制区：

| 机型 | 窗口 | 控制区顶 | 控制区高 | 左右边条 |
|---|---|---|---|---|
| GBA 240x160 @3x | 720x480 贴边 | 480 | 800 | 无（全宽） |
| GB/GBC 160x144 @4x | 640x576 贴顶 x=40 | 576 | 704 | 各 40px |
| NES 256x240 @2x | 512x480 居中 x=104 | 480 | 800 | 各 104px |

**GB/GBC 落地（T4，2026-10-07）**：
- **4x 640x576**（占屏宽 89%；3x 只有 67%）。576 > 可见区 480 → 视口 **贴顶**（`max(0,...)`），
  否则居中会算出 top=-48 切画面；映射表 `map_viewport_to_source_x/y` 必须**按视口填**（不是按可见区 480），
  否则 480..575 行残留旧映射 → 画面下半截横向拉错。
- **去掉 L/R 肩键行**：GB/GBC 是单机时代掌机。判据 `rg_touch_has_shoulders()` / `rg_touch_key_hidden()`
  （在 `rg_touch_skin.h`，**两侧唯一真源**）；消费点 = overlay 建 btns（独立计数器跳过 → btn_count
  必须等于实际建出的数）+ swap_btn 不建 + 面板分区不生成 + preview-skin.py 同步。
  **键位表（targets/tab5/touch_layout.h）不动** —— 它是坐标的单一真源。
- **面板 dy 恒 0**：按键是键位表的固定坐标，面板分区框单独平移 = 与按键错位
  （GB 4x 曾算出 dy=96 → 真机将是"框在下面、键在上面"）。要挪按键就改键位表。
- 真机判据：`vp x=40 w=640 ctrl_top=576`；控制区高 704（内容 653..1232 = 579px，余 125px）。

**三个运行时量**（`rg_touch_overlay.c`）：`win_x_cur / win_w_cur / ctrl_top_cur`，
由 `rg_overlay_sync_viewport()` 从 `rg_display_get_info()->viewport` 取（`ctrl_top = viewport.top + viewport.height`）。
⚠ 面板参数、灯位推导、`dirty_panel_strip` 的整条矩形**全部**用它们；`RG_OVERLAY_CTRL_TOP` 只作
"视口未就绪"的兜底。**别再写死 480** —— 那就是"换机型后凹槽/铭牌全错位"的来源。
面板按本次视口记忆（`panel_vp_x/w/top`），视口一变就 `panel_installed=false` 重装并整条重铺。

**判据日志（换机型先看这两行）**：
```
display: zoom source 240x160 requested x3 → x3 (720x480)      ← ZOOM 分支的降倍结果
touch skin: panel installed for 'gba' (skin ...), vp x=0 w=720 ctrl_top=480, screen 720x480
touch skin: panel rebuilt for skin D / console 'gba' (ctrl_top=480, badge_y=965, 93 ms)
```

**ZOOM 超屏自动降倍**（`rg_display.c`）：ZOOM 是固定倍数，按机型算出来的窗口可能比逻辑屏还大
（NES 3x = 768x720 > 720x480 → 居中会得到**负** left/top、画面切边）。现在在"不超屏"前提下取
最大整数倍（NES → 2x = 512x480）。GBA/GB 不超屏 → 结果与改前完全相同（回归证据：`badge_y=965` 逐位不变）。

## 单 app 合并形态（2026-10-07 定案，取代"多 app"成为默认发布形态）

**为什么合并**：第三方启动器（M5Launcher 那类"只装一个 app"的）装多 app 镜像只会拿到第一个
app（菜单），核心全丢。合并后整个镜像就是**一个 app**，装一个 = 有全部机型。

**合并了什么**：`launcher`（菜单） + `gbsp`（GBA） + `retro-core` 的 8 个核心
（gnuboy=GB/GBC、nofrendo=NES、pce-go、smsplus=SMS/GG/COL、gw-emulator、handy=Lynx、snes9x）。
实测镜像 **2,097,152 B**，比三个独立 app 相加（3.6MB）还小 —— 单 app 是一份链接，
`retro-go`/IDF/驱动的公共代码只算一次。

**机制（4 处，见 ESP32-经验沉淀 §193）**：
1. 入口改名：`launcher/components/retro-core-main/`（搬 retro-core 的 main，
   `-Dapp_main=rg_core_main_multi`；GBA 那个仍是 `rg_core_main`）。
2. NVS 待续标志带"哪一套"：`RG_SINGLE_APP_CORE_NONE/GBA/MULTI`（rg_system.h），
   由 `update_boot_config()` 按 configNs 决定，`app_main` 据此选入口。
3. `rg_system_have_app()` 认**多个**核心名（否则菜单里 GB/NES 那批游戏全空）。
4. 组件目录**逐个列**（避开 `retro-core/components/launcher` 的同名组件）。

**真机验证（2026-10-07）**：菜单出现 GBC 入口 → 点进 → `switch_app retro-core (gbc)` →
重启 → 核心读 `gbc.json`（**只有核心会读它，菜单不读**，这是分发起效的铁证）；
GBC ↔ GBA 双向切换均正常。

**缩放口径（同见 §194）**：规则是"**尽量取最大的整数放大倍数**"，唯一约束是放得下：
```c
max_zoom = min(screen_width / src_width, RG_DISPLAY_MAX_WINDOW_HEIGHT / src_height)
zoom     = min(用户的倍数设置 custom_zoom, max_zoom)
```
- `RG_DISPLAY_MAX_WINDOW_HEIGHT = 620`（= 物理屏 1280 − 控制区最小 660，由底排按键底 1232 反推）
- `RG_DISPLAY_DEFAULT_CUSTOM_ZOOM = 4.0`（= Tab5 上限；它只是上限的进一步收紧，用户调小才生效）
- 落点全是**算出来的**：GB/GBC **4x**(640x576，控制区余 704)、GBA **3x**(720x480，余 800)、
  NES **2x**(512x480，余 800)
- ⚠ **别拿逻辑屏高（display.screen.height = 480）当高度上限** —— 那是"GBA 满宽画面"的高度，
  控制区长在物理屏 1280 上。用 480 去减会把 GB 4x 一路砍到 **1x**（2026-10-07 真实翻车，
  且递减式 `while (zoom--)` 把上限写错伪装成了"正常成功"）。物理屏高常量是 `RG_SCREEN_HEIGHT`。

**凹槽判据（与缩放联动）**：画凹槽的门槛是"**边条宽度 ≥ `RG_TOUCH_PANEL_GROOVE_MIN_SIDE`(64px)**"，
由窗口 `win_x` 算出来，不按机型名硬编码 ——
GBA 0px 不画、**GB/GBC 4x 的 40px 不画（用户定：这里不做元素，留黑边）**、NES 104px 才画 bezel。
⚠ 倍数是会变的（谁改了缩放，边条宽度自动跟着变），所以判据必须挂在"宽度"上而不是机型上。
其余面板元素（分区框/铭牌/圆灯/分界线）无需随缩放改：分区框按**按键坐标**推、铭牌与灯按控制区
重新取中、分界线画在窗口底边，都自动跟随视口。

### 机型按键能力（2026-10-07）

| 机型 | 肩键 L/R | X/Y |
|---|---|---|
| GBA | 有 | 用（调换 L/R 之后的 ABXY 组） |
| GB/GBC | **无**（隐藏） | **连发**：Y=连发 A、X=连发 B |
| NES | **无**（隐藏；`NES_PAD_*` 里根本没有 L/R，是真死键） | **连发**：同上 |

- 无肩键 → `rg_touch_has_shoulders()`（rg_touch_skin.h）返回 false：不绘制、不命中、不进分区框，
  "X/Y↔L/R 调换"那颗按钮同去。**键位表（touch_layout.h）不动**，它仍是坐标唯一真源。
- 连发 = `rg_input_apply_turbo()`（rg_input.c/.h 单一真源）：每 `RG_TURBO_PERIOD`(5) 帧翻转，
  一个周期 10 帧 ≈ **每秒 12 次**（中速）。调用点只有 `retro-core/main/main_gbc.c` 与 `main_nes.c`。
  ⚠ **SNES 不能加**（X/Y 是真按键）；PCE/SMS/GG/GW 本次未动。
  ⚠ 掩码里的 X/Y 保留（只多合成 A/B），菜单/热键判断不受影响。

---

### 🟢 最新（2026-10-07）：v0.4.7 已发布（GitHub ✅ + M5Burner 待审）

**主题**：11 机种 + 四套触摸皮肤 + 按键按真实手柄 + SNES 读档崩溃修复 + 启动器首页新手引导卡。

**① 启动器首页新手引导卡（用户要求）**
- 用户照片反馈「新用户看到的第一个界面……建议美化一下，加双语提示」→ 先出 PC 模拟图（`tools/mock-launcher-hint.py`，含真机 12×12 点阵字渲染）让用户挑，选「提示卡」方案。
- 画在 **carousel 分支**（冷启动首页），**仅 `short_name == "gba"` 时显示**（用户定：只有 GBA 需要）。
- 标题改**贴顶**（原来传 `(gui.height-HEADER_HEIGHT)/2 = 215` 是垂直居中，真机跑到屏幕 17% 处，用户说不对）；
  卡片放剩余区域正中。卡片 600x204，高度按 4 行文字算（之前 186/200 时第 4 行被底边截掉，**真机照片才发现**）。
- **坐标系澄清**：`gui.height = rg_display_get_height()` = **逻辑屏 480**；launcher 的 GUI 显示在物理屏
  **上 480px**（占 37.5%），下面留给触摸按键。换算：逻辑 y → 物理百分比 = y/1280。
- **坑（-Werror=address）**：`paths.roms` 与 `short_name` 都是 **char 数组**，地址恒非 NULL，
  不能写 `x ? x : ""` 或 `!x ||`，否则编译直接失败（踩了两次）。

**② 发布**
- GitHub：公开仓补 `CHANGELOG_CN.md`（新建，双语齐了）、CHANGELOG 补 v0.4.2~0.4.7、
  README/README_CN 的 Status 与 Controls 段更新（原来还写着「其它机种 ❌ 未移植」）、ROADMAP 重写（去掉已完成项）。
  代码用 `tools/publish-github.sh` 同步（`fb29183`），tag `v0.4.7`，
  Release <https://github.com/andjiang0083/retro-go-tab5/releases/tag/v0.4.7>（4 附件：merged / single-app / SHA256 / 封面）。
- M5Burner：条目 `retro-go Tab5`（fid `2105333864932450305`）追加 **0.4.7**，
  vid `2107726805810421761`，**PENDING**，上传 2026-10-07T14:56:46+08:00，bin `retro-go-tab5-0.4.7-merged.bin`。
- ⚠️ **审核积压**：该条目现有 **4 条 PENDING**（0.4.4 / 0.4.5 / 0.4.6 / 0.4.7），线上仍是 **0.4.3**。
  国庆假期审核停摆，节后再看。

**③ 本轮新增的两个操作坑（已记 `~/esp32/ESP32-经验沉淀.md` §200）**
- **M5Burner 新版本页的上传是「拖拽」组件**：`DOM.setFileInputFiles` 能把文件塞进 input、页面上也显示文件名，
  但 **React 的 onChange 收不到**，提交后等于没传。正确做法：读 `input.files` 构造 `DataTransfer`，
  对上传容器依次派发 `dragenter`/`dragover`/`drop`（`DragEvent` + `dataTransfer`）→ 封面区立刻显示文件名。
- **burner 的接口要 `Authorization: Bearer <localStorage['m5burner.accessToken']>`**，`credentials:'include'` 不够
  （cookie 为空，会 401）。版本级列表在 `/api/v1/users/me/firmwares`（**`rows`**，每行一个版本，
  **含 PENDING**）；`/api/v1/firmwares/<fid>/versions` 只列 PUBLISHED，会把在审的误判成「没上架」。

**发布产物**：`dist/m5burner-0.4.7/`（merged 3,801,088 B / sha256 `bd7352d5…`；
single-app 2,097,152 B / sha256 `dc151e0c…`；字段表 / 双语 changelog / description / 封面）。
