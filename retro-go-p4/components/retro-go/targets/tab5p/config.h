/* Configuration for M5Stack Tab5 (ESP32-P4)
 *
 * 显示：官方 Tab5 BSP（vendor/m5stack_tab5）+ MIPI DSI/ST7123，走 RG_SCREEN_DRIVER 2。
 * 面板原生 720x1280 竖屏；逻辑画面 1280x720 横向，90° 映射在驱动里做（见驱动头部注释）。
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
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_HIGHSPEED
/* Tab5 不做 SD 卡槽的片上 LDO 供电申请（卡槽固定供电，本来就读得到卡）。
 * 原因见 rg_storage.c 里那段注释：显示初始化前申请片上 LDO 会卡死 DSI PHY 上电。 */
#define RG_STORAGE_SDMMC_ONCHIP_LDO 0
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
#define RG_SCREEN_DRIVER            2   // 2 = MIPI DSI (Tab5 BSP)，0=SPI, 99=SDL2
#define RG_SCREEN_BACKLIGHT         1
/* 逻辑分辨率（用户自然持机的横向视角）。物理面板是 720x1280 竖屏，
 * 驱动按 90° 映射写入，所以这里必须是横向的那一组数。 */
#define RG_SCREEN_WIDTH             1280
#define RG_SCREEN_HEIGHT            720
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}  // Left, Top, Right, Bottom
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}  // Left, Top, Right, Bottom
#define RG_SCREEN_PARTIAL_UPDATES   1
/* 无 SPI 命令序列：面板初始化在驱动里走 Tab5 BSP（lcd_init 会调用本宏） */
#define RG_SCREEN_INIT()

/* 游戏缩放的默认值：3x 整数缩放（240x160 -> 720x480 居中）。
 * 这是"触摸按键不压画面"的前提：按 3x 计算游戏区占 x[280,1000) y[120,600)，
 * 四周正好空出 margin 给虚拟按键（见下方 RG_GAMEPAD_TOUCH_MAP）。
 * ⚠ 必须用 ZOOM：FIT 会变成 1080x720（上下零 margin）、FULL 直接拉满 1280x720，
 *   这两种模式按键都会压在游戏画面上。custom_zoom 上限也要放开（上游硬夹 2.0）。 */
#define RG_DISPLAY_DEFAULT_SCALING     RG_DISPLAY_SCALING_ZOOM
#define RG_DISPLAY_DEFAULT_CUSTOM_ZOOM 3.0
#define RG_DISPLAY_MAX_CUSTOM_ZOOM     4.0


/****************************************************************************
 * Input — Tab5 只有 Reset/Boot，没有用户按键；用触摸屏当虚拟手柄            *
 *                                                                          *
 * 坐标是**逻辑横向空间**（1280x720，即用户实际看到的画面），命中区 = 中心+尺寸 *
 *                                                                          *
 * 硬约束：命中区**不得压到游戏画面**。按 3x 整数缩放（240x160 -> 720x480 居中）*
 * 计算，游戏区占 x[280,1000) y[120,600)，四周空白 margin 为：               *
 *      左 280px / 右 280px / 上 120px / 下 120px                            *
 * 本表所有命中区都留了 >=8px 间隙，只落在 margin 内。                        *
 *                                                                          *
 * ⚠ 因此游戏缩放模式必须是 ZOOM(custom_zoom=3)：FULL 会拉满 1280x720（零 margin），*
 *   FIT 会变成 1080x720（上下零 margin）——这两种模式下按键都会压到画面。      *
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
 * 内存代价：驱动里两个 LCD_BUFFER_LENGTH 大小的缓冲 = 2 × 40KB。 */
#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * 16)

/* GUI 字体放大倍数：默认字体 VeraBold11 渲染高度 13px，在 1280x720 上太小（实机反馈）。
 * 3 倍 = 39px。改这里即可调整（2 = 26px，3 = 39px）。 */
#define RG_GUI_FONT_SCALE 3

#if RG_ENABLE_TOUCH_GAMEPAD
/* 键位表抽到 touch_layout.h —— 命中判定 / 可视层绘制 / PC 预览三方共用一份，
 * 抄成两份必然漂移（表现：画的和点的不是一回事）。改键位改那个文件。 */
#include "touch_layout.h"
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
