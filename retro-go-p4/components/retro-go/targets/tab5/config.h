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
#define RG_AUDIO_USE_EXT_DAC        0   // 0 = Disable


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
#define RG_GAMEPAD_TOUCH_MAP { \
    /* 十字键：左侧 margin 竖列，单区 92x92，中心 (140,420) */ \
    {RG_KEY_UP,    140, 328,  92, 92}, \
    {RG_KEY_DOWN,  140, 512,  92, 92}, \
    {RG_KEY_LEFT,   54, 420,  92, 92}, \
    {RG_KEY_RIGHT, 226, 420,  92, 92}, \
    /* 动作键：右侧 margin 竖列，单区 92x92（上排 Y/X，下排 B/A） */ \
    {RG_KEY_Y, 1134, 374, 92, 92}, \
    {RG_KEY_X, 1226, 374, 92, 92}, \
    {RG_KEY_B, 1134, 466, 92, 92}, \
    {RG_KEY_A, 1226, 466, 92, 92}, \
    /* 肩键：上方 margin 左右角，单区 200x96 */ \
    {RG_KEY_L,  140, 60, 200, 96}, \
    {RG_KEY_R, 1140, 60, 200, 96}, \
    /* 系统键：下方 margin，单区 140x72 */ \
    {RG_KEY_SELECT,  520, 660, 140, 72}, \
    {RG_KEY_START,   700, 660, 140, 72}, \
    {RG_KEY_MENU,   1150, 660, 140, 72}, \
}
/* 若真机上触摸方向不对（点左选中右之类），改这个变换，不用动读点逻辑 */
#define RG_TOUCH_PHYS_W 720
#define RG_TOUCH_PHYS_H 1280


/****************************************************************************
 * Miscellaneous                                                            *
 ****************************************************************************/
/* 没有 RG_RECOVERY_BTN：Tab5 无用户按键，进不了 recovery（P2 再决定用触摸长按替代） */
#define RG_CUSTOM_PLATFORM_INIT() \
    /* Arbitrary code executed very early during retro-go init */

// See components/retro-go/config.h for more things you can define here!
