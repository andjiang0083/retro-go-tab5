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
#define RG_SCREEN_DRIVER            3   // 3 = MIPI DSI 竖屏线性（本分支专用，无旋转）
#define RG_SCREEN_BACKLIGHT         1
/* 逻辑分辨率 = **面板原生方向**（720x1280 竖屏），驱动侧是线性 1:1 映射
 * （见 drivers/display/mipi_dsi_tab5_p.h）。
 * ⚠ 竖屏分支下这里是"竖着的那一组数"，不是横屏版 1280x720 的口径 ——
 *   横屏版才需要"驱动 90° 映射 + 这里填横向数"。RG_SCREEN_ROTATE 保持 0。 */
#define RG_SCREEN_WIDTH             720
#define RG_SCREEN_HEIGHT            1280
#define RG_SCREEN_ROTATE            0
/* 【竖屏分支】游戏画面锚定顶部 y=0..480（720x480 顶满宽度），
 * 下方 800px 全给虚拟按键 —— 与 touch_layout.h 的分区一致。 */
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 800}  // Left, Top, Right, Bottom
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}  // Left, Top, Right, Bottom
#define RG_SCREEN_PARTIAL_UPDATES   1
/* 无 SPI 命令序列：面板初始化在驱动里走 Tab5 BSP（lcd_init 会调用本宏） */
#define RG_SCREEN_INIT()

/* 游戏缩放的默认值：3x 整数缩放（240x160 -> 720x480，**锚定顶部**）。
 * 这是"触摸按键不压画面"的前提：游戏区占满宽度、位于 y[0,480)，
 * 控制区是下方 y[480,1280)（键位表里所有命中区都在 y>=500）。
 * ⚠ 必须用 ZOOM：FULL 会把画面拉到满屏、FIT 会另算比例，两者都会让按键压到画面上。
 *   custom_zoom 上限也要放开（上游硬夹 2.0）。 */
#define RG_DISPLAY_DEFAULT_SCALING     RG_DISPLAY_SCALING_ZOOM
#define RG_DISPLAY_DEFAULT_CUSTOM_ZOOM 3.0
#define RG_DISPLAY_MAX_CUSTOM_ZOOM     4.0


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
 * 32 行一块 = 46KB，正好装进 48KB 的片内 SRAM 暂存(tab5_stage)，推送次数减半。 */
#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * 32)

/* GUI 字体放大倍数：默认字体 VeraBold11 渲染高度 13px，在 1280x720 上太小（实机反馈）。
 * 3 倍 = 39px。改这里即可调整（2 = 26px，3 = 39px）。 */
#define RG_GUI_FONT_SCALE 3

#if RG_ENABLE_TOUCH_GAMEPAD
/* 键位表抽到 touch_layout.h —— 命中判定 / 可视层绘制 / PC 预览三方共用一份，
 * 抄成两份必然漂移（表现：画的和点的不是一回事）。改键位改那个文件。 */
#include "touch_layout.h"
/* 【竖屏分支】触摸坐标换算覆盖：
 * rg_input.c 默认实现是横屏的 90° 逆映射（lx=py, ly=phys_w-1-px）。
 * 竖屏下显示驱动的映射是恒等的（px=lx, py=ly），触摸必须同向，否则
 * 手指位置会被换算到旋转后的逻辑点、落在命中区之外 —— 表现就是
 * 「按键画出来了但按不动」。 */
#define RG_TOUCH_LOGICAL_FROM_PHYS(px, py, lx, ly) \
    do { (lx) = (px); (ly) = (py); } while (0)

#define RG_GAMEPAD_TOUCH_MAP RG_TAB5_TOUCH_MAP
/* 若真机上触摸方向不对（点左选中右之类），改这个变换，不用动读点逻辑 */
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
#define RG_TEST_FLUSH_CACHE 0

/* 2026-10-06 临时：真机"输入 vs 显示"分辨探针（DIAG_CURSOR/DIR/FB）。验证后删除。 */
/* 2026-10-06 无人值守测试用的诊断探针，验证完毕，默认关闭（要复现再打开）。 */
#ifndef RG_GBA_DIAG
#define RG_GBA_DIAG 0   /* 验证完成：关闭诊断探针与脚本按键，出干净发行版 */
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
#define RG_GBA_DIAG_SPAM 1
#endif

#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 0.001f)/* 充电判定阈值（mA，取分流电流绝对值）：小于这个充电电流不算"在充电"，
 * 免得不插充电器时被噪声抖成闪烁。官方口径：分流电流**为负**表示在充电。 */
#define RG_TAB5_CHARGE_CURRENT_MA   40
#endif
