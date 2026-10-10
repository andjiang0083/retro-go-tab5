/* Configuration for M5Stack Tab5 (ESP32-P4)
 *
 * 显示：官方 Tab5 BSP（vendor/m5stack_tab5）+ MIPI DSI/ST7123，走 RG_SCREEN_DRIVER 2。
 * 【竖屏分支】面板原生 720x1280 竖屏 = 逻辑画面同向，**无旋转**、顺序读写
 * （横屏主线为逻辑 1280x720 + 90° 映射；本分支走驱动编号 3 = mipi_dsi_tab5_p.h）。
 * 构建：python rg_tool.py --target tab5 build-img launcher gbsp --no-networking
 */

/****************************************************************************
 * Target definition                                                        *
 ****************************************************************************/
#define RG_TARGET_NAME             "M5Stack Tab5"


/****************************************************************************
 * Storage (SD 卡, SDMMC 4-bit) — 引脚与 Tab5 硬件一致                       *
 ****************************************************************************/
#define RG_STORAGE_ROOT             "/sd"
/* 【A/B 实验结论 · 2026-10-06】厂商配方（slot0 + 4-bit + 片上 LDO chan4）与被别的固件改坏的
 * 卡槽状态无关：真机实测，卡进入"CMD1 不应答（0x107）"状态后，软件侧（含启用 LDO chan4 重试）
 * **救不回来**，只有重插卡 + 真断电才恢复 ⇒ 依据"不改动能用的东西"，仍用原配方
 * （slot1 + 1-bit + 不主动申请 LDO），只在**首次挂载失败**时启用 LDO 回退（见 rg_storage.c）。
 * 这个开关保留下来仅作为可复现的对照配置，默认 0。 */
#define RG_STORAGE_SDMMC_VENDOR_RECIPE 0
#if RG_STORAGE_SDMMC_VENDOR_RECIPE
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_0
#define RG_STORAGE_SDMMC_ONCHIP_LDO 1
#define RG_STORAGE_SDMMC_WIDTH      4
#else
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
/* Tab5 不做 SD 卡槽的片上 LDO 供电申请（卡槽固定供电，本来就读得到卡）。
 * 原因见 rg_storage.c 里那段注释：显示初始化前申请片上 LDO 会卡死 DSI PHY 上电。 */
#define RG_STORAGE_SDMMC_ONCHIP_LDO 0
#define RG_STORAGE_SDMMC_WIDTH      1
#endif
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_HIGHSPEED
/* 【但硬件状态会被别的固件改掉 · 2026-10-06】卡槽 I/O 的供电其实是片上 LDO 的 chan4
 * （厂商 BSP：BSP_LDO_PROBE_SD_CHAN=4 / 3300mV，"LDO_VO4 is used as the SDMMC IO power"），
 * 而这份状态**跨软复位保留**：被会自己配 LDO 的固件（如 M5Launcher）改过之后，我们的 SD 初始化
 * 就永远等不到 CMD1 应答（0x107 超时，屏幕提示 "SD Card Error / Storage mount failed"）。
 * rg_storage.c 里在"第一次挂载失败"时才用这个通道号配 LDO 重试 —— 正常开机路径不受影响。 */
#define RG_STORAGE_SDMMC_LDO_CHAN   4
#define RG_GPIO_SDMMC_CLK           GPIO_NUM_43
#define RG_GPIO_SDMMC_CMD           GPIO_NUM_44
#define RG_GPIO_SDMMC_D0            GPIO_NUM_39
#define RG_GPIO_SDMMC_D1            GPIO_NUM_40
#define RG_GPIO_SDMMC_D2            GPIO_NUM_41
#define RG_GPIO_SDMMC_D3            GPIO_NUM_42


/****************************************************************************
 * Audio — P1 全禁（ES8388 要走新 I2S API 适配，放到最后阶段）                *
 ****************************************************************************/
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable
#define RG_AUDIO_USE_EXT_DAC        0   // 0 = Disable（老 i2s.c 不支持 MCLK，用不了）
#define RG_AUDIO_USE_TAB5_CODEC     1   // ES8388 via M5 BSP: MCLK=30 BCLK=27 WS=29 DOUT=26


/****************************************************************************
 * Video — MIPI DSI / ST7123, 走官方 Tab5 BSP                                *
 ****************************************************************************/
/* 【屏幕方向开关】0 = 竖屏（默认，= 现状，行为零变化） 1 = 横屏
 * ---------------------------------------------------------------------------
 * 横屏形态 = 老线（M5Burner 0.2/0.3）那套：逻辑画布 1280×720、驱动做 90° 映射，
 * 游戏窗仍是 **720×480 居中**（x[280,1000) y[120,600)），四周留白全给虚拟按键。
 *
 * 两种方向的差别**只有四处**：逻辑画布尺寸 / 驱动映射方向 / 键位表 / 视口上限；
 * 核心、缩放、滤镜、存档、封面完全共用。详见 docs/ORIENTATION-SWITCH-EVAL.md。
 *
 * 构建横屏镜像：RG_TAB5_ORIENTATION=1 tools/build-tab5-skin.sh
 * （⚠ 构建脚本每次都会显式传 0/1 —— CMake 缓存变量是"粘"的，不显式传会串味。） */
#ifndef RG_TAB5_ORIENTATION
#define RG_TAB5_ORIENTATION 0
#endif

/* 【PPA 传输模式实验开关】2026-10-10，横屏驱动（mipi_dsi_tab5.h）专用。
 * 目的：回答"按块 PPA 慢 25 倍"里有多少是 blocking 的等待/提交成本、多少是硬件本体
 * （方案与判据见 docs/SPEC-PPA-NONBLOCK-EXPERIMENT.md）。
 *   0 = PPA 关（**默认，= 现状**，CPU 转置；发行/日常行为零变化）
 *   1 = 按块 PPA **非阻塞**（事件回调 semaphore + 1 深流水线；叠加层推迟到收块之后合成）
 *   2 = 按块 PPA **BLOCKING**（= 当年 25x 那个形态，用作"同配置对照"，把等待成本单独拆出来）
 *   3 = 按块 PPA **BLOCKING + 叠加层合成到源缓冲**（P0 首版，2026-10-10）
 *       ⛔ **实测失败，不要使用**：显示路径本身达标（稳态 0.91ms/块、叠加层混合只剩 44µs），
 *          但**系统级崩塌** —— 240s 里模拟器只推进约 6 秒的量（对照档 ~225 行心跳、本档 6 行），
 *          无 panic / 无 PPA 报错 / 无重启。已排除 UART 刷屏（SPAM=0 复现）、对齐、存储落点。
 *          未解释的规律："每块不再碰帧缓冲"与崩塌强相关（当年 D 组同样掉到 ~2fps）。
 *          失败注记与待验证假设见 `drivers/display/mipi_dsi_tab5.h` 里 `RG_TAB5_PPA_MODE == 3` 段。
 *       —— 2 与 3 只差"叠加层在哪合成"这一个变量：
 *          2 = 写面板帧缓冲（裁剪矩形=整屏 ⇒ 每块都混合全部 13 个按键，≈30.4ms/块，实测真身）
 *          3 = 写本块的**暂存源缓冲**（原点=本块，只混合本块内的按键；随旋转被 PPA 转过去）
 *       配套：mode 3 的暂存块换序成小端 ⇒ PPA 用 .byte_swap=false（由 src_le 自动推导）。
 * ⚠ 只影响横屏驱动。竖屏驱动（mipi_dsi_tab5_p.h）的 PPA 路径不读这个开关。 */
#ifndef RG_TAB5_PPA_MODE
#define RG_TAB5_PPA_MODE 0
#endif

/* 运行时几何表：**必须在这里 include** —— 下面这些宏（RG_SCREEN_WIDTH 等）展开成
 * rg_geom()->…，而宏是在**调用方 TU** 展开的，所以每个用到它们的翻译单元都要先看到声明。
 * （config.h 本身被所有 TU 经 rg_system.h 间接包含 ⇒ 挂在这里最省事。） */
#include "geom.h"

/* ════════════ 屏幕几何：**运行时按方向取**（0.4.9 单 app）════════════
 * 原来这里是一对 `#if RG_TAB5_ORIENTATION == 1 … #else … #endif`：一次编译只装一个方向。
 * 单 app 形态要一个镜像同时支持横竖 ⇒ 几何改成运行时值（两行表在 targets/tab5/geom.h）。
 *
 * 手法（关键）：下面把宏**定义成"读运行时几何的表达式"**（如 RG_SCREEN_WIDTH = rg_geom()->w），
 * 于是所有既有调用点**一字不改**就变成运行时取值；只有少数"必须是编译期常量"的地方
 * （数组长度 / 静态初始化）需要单独处理 —— 编译器会把它们全部报出来，不靠人找。
 * 值本身没变：横屏那套值原样搬进 geom.h 的 [1] 行、竖屏搬进 [0] 行。
 * 双 app 形态下 rg_orient_active() 返回运行分区标签 ⇒ 取到的值 = 改造前的编译期宏（零回归）。 */

/* —— 两方向**相同**的部分，保持普通宏 —— */
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
#define RG_SCREEN_PARTIAL_UPDATES   1
/* 无 SPI 命令序列：面板初始化在驱动里走 Tab5 BSP（lcd_init 会调用本宏） */
#define RG_SCREEN_INIT()
/* 游戏缩放两方向都用 ZOOM（是否锁死在设置里由 rg_geom()->lock_scaling 决定；
 * 倍数上限两方向不同，见下）。 */
#define RG_DISPLAY_DEFAULT_SCALING     RG_DISPLAY_SCALING_ZOOM

/* —— 按方向变化的 9 项：值在 targets/tab5/geom.h —— */
/* ⚠ RG_SCREEN_DRIVER **必须保持编译期常量** —— 它被 rg_display.c 用 `#if` 选"包含哪份驱动头"。
 * 双 app：2 = 横 / 3 = 竖（与改造前逐字相同）；单 app：4 = "两份都编、运行时由分发层选"。
 * 运行时想知道"当前生效的是哪份后端"，用 rg_geom()->driver（= 2 或 3）。 */
#if defined(RG_SINGLE_APP)
#define RG_SCREEN_DRIVER               4
#elif RG_TAB5_ORIENTATION == 1
#define RG_SCREEN_DRIVER               2
#else
#define RG_SCREEN_DRIVER               3
#endif
#define RG_SCREEN_WIDTH                (rg_geom()->w)
#define RG_SCREEN_HEIGHT               (rg_geom()->h)
#define RG_SCREEN_VISIBLE_AREA         {rg_geom()->vis.l, rg_geom()->vis.t, rg_geom()->vis.r, rg_geom()->vis.b}
#define RG_SCREEN_VISIBLE_AREA_NO_SHLD {rg_geom()->vis_ns.l, rg_geom()->vis_ns.t, rg_geom()->vis_ns.r, rg_geom()->vis_ns.b}
#define RG_DISPLAY_DEFAULT_CUSTOM_ZOOM (rg_geom()->def_custom_zoom)
#define RG_DISPLAY_MAX_CUSTOM_ZOOM     (rg_geom()->max_custom_zoom)
#define RG_DISPLAY_MAX_WINDOW_HEIGHT   (rg_geom()->max_window_h)
#define RG_DISPLAY_LOCK_SCALING        (rg_geom()->lock_scaling)


/****************************************************************************
 * Input — Tab5 只有 Reset/Boot，没有用户按键；用触摸屏当虚拟手柄            *
 *                                                                          *
 * 坐标是**逻辑空间 720x1280 竖屏**（= 用户实际看到的方向），命中区 = 中心+尺寸 *
 *                                                                          *
 * 硬约束：命中区**不得压到游戏画面**。按 3x 整数缩放（240x160 -> 720x480）   *
 * 计算，游戏区占满宽度、位于**顶部** y[0,480)，控制区是下方 y[480,1280) ——    *
 * 800px 高，比横屏版两侧各 280px 的 margin 宽裕得多。                        *
 * 本表所有命中区都在 y>=500，与游戏区之间留有大片空白。                      *
 *                                                                          *
 * ⚠ 因此游戏缩放模式必须是 ZOOM(custom_zoom=3)：FULL 会把画面拉满屏，        *
 *   按键就会压到画面上。（横屏版的"上下零 margin"问题在竖屏下不存在。）      *
 * （launcher 菜单是全屏 UI，那时按键压到菜单是正常的、也是必需的。）          *
 ****************************************************************************/
/* P2 触摸虚拟手柄总开关：P1 显示验证阶段置 0，避免触摸初始化的问题干扰显示判定；
 * 一次刷机 = 一个可见变化。P2 阶段改成 1 即可。 */
#define RG_ENABLE_TOUCH_GAMEPAD 1

/* 虚拟按键可视层：把命中区画到屏幕上（0 = 完全不编，回到"盲区"模式）。
 * 实现在 rg_touch_overlay.c（目标无关的共享模块），由显示驱动在"推给面板前"合成
 * ——放在显示层是为了不被 GUI 立即模式的重绘覆盖（实机表现为按键闪烁，27b44b4 修过）。
 * 标签/透明度/按下反馈都在那个模块里；运行时开关与透明度档位存 NVS（NS_GLOBAL），
 * 入口在设置菜单 →「虚拟手柄」。 */
#define RG_TOUCH_OVERLAY 1

/* 每帧"算一块推一块"的块大小（像素）。默认 RG_SCREEN_WIDTH*4=4 行 => 一帧 180 次推送，
 * 播游戏时面板 DMA 会持续忙、触发 tab5_draw 的重试风暴。这里放到 16 行（一帧 45 次）。
 * 内存代价：驱动里两个 LCD_BUFFER_LENGTH 大小的缓冲 = 2 × 46,080B（720×32×2B ≈ 90KB），
 * 再加上 48KB 的片内 SRAM 暂存（驱动里的 tab5_stage），合计约 138KB 静态片内 SRAM。 */
/* 2026-09-28：16→32 行/块。真机实测每 16 行推一次 → 一帧 30 次推送，
 * DMA2D 大量丢弃(ESP_ERR_INVALID_STATE)、重试烧掉 224ms/秒。
 * 32 行一块 = 46KB，正好装进 48KB 的片内 SRAM 暂存(tab5_stage)，推送次数减半。
 * ⚠ 横屏不能用 32：块大小 = 宽×行数×2B，1280×32×2B = 80KB **超过 48KB 暂存**；
 *   横屏取 16 行 = 40KB（老线就是这个值）。 */
/* 每帧"算一块推一块"的块大小（像素）= 宽 × 行数，行数按方向取（竖 32 / 横 16；值在 geom.h）。
 * 为什么横屏不能 32：块 = 宽×行数×2B，1280×32×2B = 80KB **超过 48KB 片内暂存**（竖屏 720×32×2B
 * = 46KB 才正好装下）；横屏取 16 行 = 40KB。历史上 16 行块导致重试风暴（DMA2D 丢包、224ms/秒），
 * 所以竖屏保持 32、横屏保持 16 —— 与改造前逐字相同。
 * ⚠ 驱动里两个缓冲是**静态数组**（长度必须编译期已知）⇒ 按两方向最大值分配
 *   （LCD_BUFFER_LENGTH_MAX，见 geom.h），实际使用长度由这里按方向给。 */
#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * rg_geom()->lcd_rows)

/* GUI 字体放大倍数：默认字体 VeraBold11 渲染高度 13px，在 1280x720 上太小（实机反馈）。
 * 3 倍 = 39px。改这里即可调整（2 = 26px，3 = 39px）。 */
#define RG_GUI_FONT_SCALE 3

#if RG_ENABLE_TOUCH_GAMEPAD
/* 键位表抽到 touch_layout.h —— 命中判定 / 可视层绘制 / PC 预览三方共用一份，
 * 抄成两份必然漂移（表现：画的和点的不是一回事）。改键位改那个文件。 */
#include "touch_layout.h"
/* 触摸坐标换算：改成**运行时按方向分支**（0.4.9 单 app 一个镜像要装两个方向）。
 * 两个分支的算式分别与原实现逐字相同：
 *   横屏 = rg_input.c 的默认 90° 逆映射（lx=py, ly=phys_w-1-px，与驱动写入方向同源，
 *          老线 0.2/0.3 真机验证过）；
 *   竖屏 = 恒等映射（显示驱动就是线性的）—— 不同向的话手指会被换算到旋转后的逻辑点、
 *          落在命中区之外，表现就是「按键画出来了但按不动」。
 * 调用点（rg_input.c 的 4 处）一字不改。 */
#define RG_TOUCH_LOGICAL_FROM_PHYS(px, py, lx, ly)                                  \
    do {                                                                            \
        if (rg_orient_active() == RG_ORIENT_LANDSCAPE) {                            \
            (lx) = (py);                                                            \
            (ly) = (RG_TOUCH_PHYS_W - 1) - (px);                                    \
        } else {                                                                    \
            (lx) = (px);                                                            \
            (ly) = (py);                                                            \
        }                                                                           \
    } while (0)

/* 键位表：两张表的定义都在 touch_layout.h（单一数据源不拆文件）。
 * ⚠ 0.4.9：选表从编译期改成**运行时**（单 app 一个镜像两个方向）——
 *   RG_GAMEPAD_TOUCH_MAP     = 主表（= 本次编译方向那张，双 app 形态行为逐字不变）
 *   RG_GAMEPAD_TOUCH_MAP_ALT = 备表（另一个方向）
 *   rg_input.c 在启动时按 rg_orient_active() 把对应那张装进可变数组 keymap_touch[]，
 *   之后所有既有调用点（含按数组名展开的 UPDATE_GLOBAL_MAP 宏）都不用改。 */
#if RG_TAB5_ORIENTATION == 1
#define RG_GAMEPAD_TOUCH_MAP     RG_TAB5_TOUCH_MAP_LANDSCAPE
#define RG_GAMEPAD_TOUCH_MAP_ALT RG_TAB5_TOUCH_MAP_PORTRAIT
#else
#define RG_GAMEPAD_TOUCH_MAP     RG_TAB5_TOUCH_MAP_PORTRAIT
#define RG_GAMEPAD_TOUCH_MAP_ALT RG_TAB5_TOUCH_MAP_LANDSCAPE
#endif
/* 若真机上触摸方向不对（点左选中右之类），改这个变换，不用动读点逻辑。
 * 物理面板两种方向都一样：720×1280。 */
#define RG_TOUCH_PHYS_W 720
#define RG_TOUCH_PHYS_H 1280
#endif /* RG_ENABLE_TOUCH_GAMEPAD */


/****************************************************************************
 * Miscellaneous                                                            *
 ****************************************************************************/
/* 没有 RG_RECOVERY_BTN：Tab5 无用户按键，进不了 recovery（P2 再决定用触摸长按替代） */
#define RG_CUSTOM_PLATFORM_INIT() \
    /* Arbitrary code executed very early during retro-go init */

// See components/retro-go/config.h for more things you can define here!

/****************************************************************************
 * 电池电量读取（2026-09-29 新增）
 * ---------------------------------------------------------------------------
 * 此前本 target 完全没有电量配置 → rg_input_read_battery_raw() 直接 return
 * false → 状态栏 BATT 恒为 0。
 * 硬件参数取自 gywan94/tab5-vgbanext 的 odroid_input.c：
 *   GPIO 53 = ADC2_CH4，分压 68K(电池侧) + 100K(GND侧) → 比例 1.68
 * 百分比按锂电 3.3V(0%) ~ 4.2V(100%) 线性换算（够用；偏了再校准）。
 ***************************************************************************/
#ifndef RG_TAB5_BATTERY_CONFIGURED
#define RG_TAB5_BATTERY_CONFIGURED
/* Tab5 没有可用的电池 ADC 脚（GPIO53 在官方 BSP 里是 I2C SDA），
 * 电量走 INA226 电源监测芯片：I2C 0x41，寄存器 0x02 = Bus Voltage（1.25mV/LSB）。
 * 它挂在 BSP 主 I2C 总线上（SDA=31/SCL=32），读法见 rg_input.c 的 RG_BATTERY_DRIVER==3。 */
#define RG_BATTERY_DRIVER           3
/* 【换算口径】INA226 监测的是 **2S 锂电包**（NP-F550 7.4V, 6.0~8.4V），不是单节，
 * 所以 raw（电包 mV）不能直接套单节的 3300~4200。
 * 官方口径（M5Unified Power_Class::getBatteryLevel，杜撰不得）：
 *     mv  = busVoltage(V) * 500        // = 电包 mV / 2 = 单节平均 mV
 *     lvl = (mv - 3300) * 100 / (4150 - 3350)
 * ⚠ 注意上面官方式子里分母 3350 与分子 3300 并不自洽（原文如此）。
 * 我们采用**端点明确、单调**的映射：单节 3300mV = 0%、4150mV = 100%，即
 *     lvl = (mv - 3300) / (4150 - 3300) * 100 = (mv - 3300) / 850 * 100
 * 分母 850 与官方写法的 800 不同，但端点一致，且满电（4150mV）恰好 100% 不会溢出。
 * 要重新标定就改这两个常数（3300 = 0% 点，850 = 量程）。
 * 结果在 rg_input.c 里还会被 clamp 到 0..100（低电压/无电池时会出现负值）。 */
#define RG_BATTERY_CALC_PERCENT(raw) ((((raw) / 2.f) - 3300.f) / 850.f * 100.f)
/* 显示用电包电压（7.4V 那种，与 M5Unified getBatteryVoltage 一致），不折半 */
/* 诊断开关（2026-10-03）：=1 时把每次触点的 raw/logical 坐标与矢量判定结果打到串口。
 * 用于分辨"点没命中"与"命中了但出的键不对"。查完问题记得改回 0（会刷日志）。 */
/* 2026-10-06 验证完成：关闭触摸追踪日志。
 * 注意：rg_input.c 用 #ifdef 判定，设 0 无效，必须不定义。 */
#undef RG_TOUCH_TRACE

/* 诊断开关：记录传入 GBA 核心的按键掩码与 KEYINPUT 值；仅用于一次性真机捕获。 */
/* 2026-10-06 临时：显示层 A/B —— 关掉行级部分刷新（每帧全刷），用于判定
 * "静态菜单按键没反应"是否由行校验和过滤/丢块造成。验证后删除。 */
/* #define RG_DISPLAY_FULL_REFRESH_AB 1 */

/* 2026-10-06 临时：真机无人值守按键时间轴（时间基准 = 开机秒数，跨 launcher→gbsp 连续）。
 * 8.0s 在启动器里按 A 启动高亮 ROM；之后是游戏内的按键序列。验证完删除。 */
/* 2026-10-06 无人值守复现用：真机按键时间轴。验证完毕，默认关闭（要复现去掉注释）。 */
/* 2026-10-06 验证完成：关闭自动按键脚本（空字符串 = 关闭），恢复真机手动输入 */
#define RG_TEST_KEYS_DEVICE ""
#define RG_TEST_NO_AUTOSAVE 0   /* 恢复常规存档行为 */
/* 2026-10-10 PPA 非阻塞实验用过的静音钩子：**默认 0**（实验期曾置 1）。
 * 保留定义而非删除，是因为 rg_audio.c 里有 `#if RG_TEST_MUTE` 引用 —— 宏不存在会变成
 * 隐式 0，读代码的人分不清"故意的"还是"漏了的"。 */
#ifndef RG_TEST_MUTE
#define RG_TEST_MUTE 0
#endif
#define RG_TEST_FLUSH_CACHE 0

/* 2026-10-06 临时：真机"输入 vs 显示"分辨探针（DIAG_CURSOR/DIR/FB）。验证后删除。 */
/* 2026-10-06 无人值守测试用的诊断探针，验证完毕，默认关闭（要复现再打开）。 */
/* 2026-10-10 PPA 非阻塞实验：临时打开过（实验已结束，改回 0） */
#ifndef RG_GBA_DIAG
#define RG_GBA_DIAG 0   /* 验证完成：关闭诊断探针与脚本按键，出干净发行版。
                         * （2026-10-10 晚二轮 P0 测量期临时开过 1，测完已改回 0。） */
#endif

#ifndef RG_GBA_INPUT_TRACE
#define RG_GBA_INPUT_TRACE 0
#endif

/* 2026-10-06 临时测试钩子：强制"启动器带了存档位"那条路（真机上没人点启动器）。
 * 与启动器语义等价：加 RG_BOOT_RESUME + 指定 slot。验证完必须改回 0。 */
#ifndef RG_TEST_BOOT_RESUME
#define RG_TEST_BOOT_RESUME 0
#endif
#ifndef RG_TEST_BOOT_RESUME_SLOT
#define RG_TEST_BOOT_RESUME_SLOT 0
#endif
/* 2026-10-06 临时 A/B：=1 时关掉"第二块画面缓冲 + 轮换"，量改前的横条率。验证完删除。 */
#ifndef RG_TEST_NO_SURFACE_ROTATION
#define RG_TEST_NO_SURFACE_ROTATION 0
#endif
/* 2026-10-06 临时：诊断探针里的"每帧刷屏"部分（ANCH / DIAG_CURSOR / DIAG_DIR / DIAG_FB）。
 * 量横条时必须关掉：每帧几十行 UART 会把核心拖慢、污染显示侧测量。
 * 默认 1（行为不变），横条 A/B 时设 0 ⇒ 只留每秒一行的 DIAG_TEAR。 */
#ifndef RG_GBA_DIAG_SPAM
#define RG_GBA_DIAG_SPAM 1   /* 默认 1（行为不变）。2026-10-10 P0 测量期临时设 0：每帧几十行 UART
                              * 会拖慢核心、污染显示侧测量；两轮测量都已跑完，已改回 1。
                              * （附：SPAM=1 下 E2 那轮曾出现"f=15 后日志断流"；SPAM=0 复测
                              *   **仍卡死** ⇒ 卡死不是 UART 造成的。） */
#endif

#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 0.001f)/* 充电判定阈值（mA，取分流电流绝对值）：小于这个充电电流不算"在充电"，
 * 免得不插充电器时被噪声抖成闪烁。官方口径：分流电流**为负**表示在充电。 */
#define RG_TAB5_CHARGE_CURRENT_MA   40
#endif
