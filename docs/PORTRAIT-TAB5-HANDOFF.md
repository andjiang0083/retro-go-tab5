# Tab5 竖屏 retro-go —— 交接文档（2026-09-29 夜 更新）

## 一、当前状态

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
  v0.4.1 之后的**全项目代码走查处置**，报告见 `docs/CODE-REVIEW-v0.4.1.md`
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
### 0) ✅ v0.4.3：M5Launcher 兼容（本轮完成，已真机验证）—— 详见"一、当前状态"顶部
字库内嵌 + 单 app 形态 + 两份发布物料；只差用户本人发布。
### 0.5) ⏳ ③ SD 读不到（用户要求稍后处理）
候选修法：我们 SD 初始化 4-bit **失败后回退 1-bit**（M5Launcher 用同一组引脚、1-bit、挂 `/sdcard`）。
需要真机复现来定性：刷 M5Launcher 2.9.1 → 用它装我们的**单 app 包** → 抓我们 app 的启动日志
（区分"NVS 被重排"还是"挂载失败"）；可逆，1 分钟刷回。顺带也就验证了 Launcher 平台侧的实际安装。
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

### 2) ⏳ 发布 v0.4.2 —— 代码/真机/物料全部就绪，只差用户本人在 M5Burner 里点 Publish
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
- **当前镜像（v0.4.3，2026-09-29）**：
  - 双 app（M5Burner/esptool 主产物）：`retro-go-p4/retro-go_v0.0.1-82-g532c3_tab5.img`（2,424,832 B）
  - 单 app（M5Launcher 专用）：`retro-go-p4/retro-go_v0.0.1-82-g532c3_tab5-single.img`（1,507,328 B）
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
