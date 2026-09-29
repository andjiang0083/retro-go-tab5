#include "rg_system.h"
#include "rg_input.h"
#include "rg_touch_overlay.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include <driver/gpio.h>
#include <driver/adc.h>
// This is a lazy way to silence deprecation notices on some esp-idf versions...
// This hardcoded value is the first thing to check if something stops working!
#define ADC_ATTEN_DB_11 3
#else
#include <SDL2/SDL.h>
#endif

#if RG_BATTERY_DRIVER == 1
#include <esp_adc_cal.h>
static esp_adc_cal_characteristics_t adc_chars;
#endif

#ifdef RG_GAMEPAD_ADC_MAP
static rg_keymap_adc_t keymap_adc[] = RG_GAMEPAD_ADC_MAP;
#endif
#ifdef RG_GAMEPAD_GPIO_MAP
static rg_keymap_gpio_t keymap_gpio[] = RG_GAMEPAD_GPIO_MAP;
#endif
#ifdef RG_GAMEPAD_I2C_MAP
static rg_keymap_i2c_t keymap_i2c[] = RG_GAMEPAD_I2C_MAP;
#endif
#ifdef RG_GAMEPAD_KBD_MAP
static rg_keymap_kbd_t keymap_kbd[] = RG_GAMEPAD_KBD_MAP;
#endif
#ifdef RG_GAMEPAD_SERIAL_MAP
static rg_keymap_serial_t keymap_serial[] = RG_GAMEPAD_SERIAL_MAP;
#endif
#ifdef RG_GAMEPAD_VIRT_MAP
static rg_keymap_virt_t keymap_virt[] = RG_GAMEPAD_VIRT_MAP;
#endif

#ifdef RG_GAMEPAD_TOUCH_MAP
#ifndef CONFIG_ESP_LCD_TOUCH_MAX_POINTS
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
#endif
#ifdef ESP_PLATFORM
#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_st7123.h"
#endif
/* 触摸 IC 报的是面板物理坐标（竖屏原生 720x1280），与显示驱动的 90° 映射互逆 */
#ifndef RG_TOUCH_PHYS_W
#define RG_TOUCH_PHYS_W 720
#endif
#ifndef RG_TOUCH_PHYS_H
#define RG_TOUCH_PHYS_H 1280
#endif
/* 物理 -> 逻辑。默认：逻辑 lx = 物理 py，逻辑 ly = (PHYS_W-1) - 物理 px。
 * 若真机上出现镜像/换轴错误，只改这一个宏即可（不用动读点逻辑）。 */
#ifndef RG_TOUCH_LOGICAL_FROM_PHYS
#define RG_TOUCH_LOGICAL_FROM_PHYS(px, py, lx, ly) \
    do { (lx) = (py); (ly) = (RG_TOUCH_PHYS_W - 1) - (px); } while (0)
#endif
static rg_keymap_touch_t keymap_touch[] = RG_GAMEPAD_TOUCH_MAP;

/* 虚拟按键隐藏时的恢复入口 = 左上角那个可见的开关（几何由可视层提供：
 * rg_overlay_get_toggle_rect()，别在这里再写一份坐标）。
 * 早期版本是"左上角 100x100 长按 1.2s"的秘密手势 —— 用户找不到、等于没有入口，已换掉。 */

/* 当前按下的键（含触摸虚拟键）。可视层用它做"按下高亮"：
 * 只读输入任务维护的全局量，不做事件泵（显示路径每帧要调几十次）。
 * （定义放在 gamepad_state 声明之后，见文件下方。） */


const rg_keymap_touch_t *rg_input_get_touch_keymap(size_t *count)
{
    if (count)
        *count = RG_COUNT(keymap_touch);
    return keymap_touch;
}

#ifdef ESP_PLATFORM
static esp_lcd_touch_handle_t touch_handle = NULL;
/* Tab5 BSP 的 I2C 句柄（声明在 bsp/m5stack_tab5.h，那个 umbrella 头会拉 lvgl.h，故手写 extern） */
extern esp_err_t bsp_i2c_init(void);
extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/* 照抄 BSP 的 ST7123 触摸初始化（bsp_display_indev_init_to_st7123）：
 * 一体化屏的触摸 IC 就是 ST7123，I2C 地址 0x55，x_max/y_max 用面板物理尺寸。
 * 注意 BSP 只对外暴露了 GT911 版本的 bsp_touch_new()，所以这里自己建 panel io。 */
static esp_err_t rg_touch_init(void)
{
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK)
        return err;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = 0x55,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = {
            .disable_control_phase = 1,
        },
    };
    io_config.scl_speed_hz = 100000;
    err = esp_lcd_new_panel_io_i2c_v2(bsp_i2c_get_handle(), &io_config, &io);
    if (err != ESP_OK)
        return err;

    const esp_lcd_touch_config_t tp_config = {
        .x_max = RG_TOUCH_PHYS_W,
        .y_max = RG_TOUCH_PHYS_H,
        .rst_gpio_num = -1,   // NC on Tab5
        .int_gpio_num = 23,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    err = esp_lcd_touch_new_i2c_st7123(io, &tp_config, &touch_handle);
    if (err != ESP_OK || touch_handle == NULL) {
        esp_lcd_panel_io_del(io);
        touch_handle = NULL;
        return err != ESP_OK ? err : ESP_FAIL;
    }
    return ESP_OK;
}

/* 触摸懒加载 + 重试：
 * Tab5 的 TP_RST 由 I2C 上的 IO 扩展器控制，而扩展器是在**显示初始化**里才被初始化的
 * （见 drivers/display/mipi_dsi_tab5.h），可 rg_input_init() 跑在那之前 —— 那时触摸 IC
 * 还在复位里，探测/创建必然失败。所以改成"第一次真正读手柄时才创建"，并且失败允许重试
 * （复位释放后 IC 需要上百毫秒才在 I2C 上应答）。 */
#define RG_TOUCH_MAX_ATTEMPTS 20
static int touch_init_attempts = 0;
static int64_t touch_last_attempt = 0;

static bool rg_touch_ensure(void)
{
    if (touch_handle)
        return true;
    if (touch_init_attempts >= RG_TOUCH_MAX_ATTEMPTS)
        return false;
    /* 重试间隔 50ms（rg_system_timer 单位是微秒），避免在 IC 没醒时把 I2C 刷爆 */
    int64_t now = rg_system_timer();
    if (touch_last_attempt && (now - touch_last_attempt) < 50 * 1000)
        return false;
    touch_last_attempt = now;
    touch_init_attempts++;
    esp_err_t err = rg_touch_init();
    if (err != ESP_OK) {
        RG_LOGW("Touch init attempt %d/%d failed (0x%x)\n", touch_init_attempts, RG_TOUCH_MAX_ATTEMPTS, err);
        return false;
    }
    RG_LOGI("Touch ready (after %d attempt(s)).\n", touch_init_attempts);
    return true;
}
#else
/* 宿主（SDL2 预览）没有触摸驱动：可视层只需要键位表，命中判定走键盘 */
static bool rg_touch_ensure(void) { return false; }
#endif /* ESP_PLATFORM */
#endif /* RG_GAMEPAD_TOUCH_MAP */
static bool input_task_running = false;
static bool input_task_exited = false;   /* 任务真正退出后置真：deinit 要先等它，见 rg_input_deinit */
static uint32_t gamepad_state = -1; // _Atomic

/* 当前按下的键（含触摸虚拟键）。可视层用它做"按下高亮"：
 * 只读输入任务维护的全局量，不做事件泵（显示路径每帧要调几十次）。 */
uint32_t rg_input_get_pressed_mask(void)
{
    return (gamepad_state == (uint32_t)-1) ? 0 : (gamepad_state & RG_KEY_ALL);
}
static uint32_t gamepad_mapped = 0;
static rg_battery_t battery_state = {0};
static int battery_state_prev_charging = -1;   /* 去抖用：上一次"已确认"的充电状态（-1 = 还没初始化） */

#define UPDATE_GLOBAL_MAP(keymap)                 \
    for (size_t i = 0; i < RG_COUNT(keymap); ++i) \
        gamepad_mapped |= keymap[i].key;          \

#ifdef ESP_PLATFORM
static inline int adc_get_raw(adc_unit_t unit, adc_channel_t channel)
{
    if (unit == ADC_UNIT_1)
    {
        return adc1_get_raw(channel);
    }
    else if (unit == ADC_UNIT_2)
    {
        int adc_raw_value = -1;
        if (adc2_get_raw(channel, ADC_WIDTH_MAX - 1, &adc_raw_value) != ESP_OK)
            RG_LOGE("ADC2 reading failed, this can happen while wifi is active.");
        return adc_raw_value;
    }
    RG_LOGE("Invalid ADC unit %d", (int)unit);
    return -1;
}
#endif

#if RG_BATTERY_DRIVER == 3
/* ---- INA226 电量芯片（Tab5 专用）-------------------------------------------
 * 读法以官方实现为准（M5Tab5-UserDemo hal_esp32.cpp + M5Unified Power_Class）：
 * INA226 挂在 **BSP 的主 I2C 总线**上（`bsp_i2c_get_handle()`，SDA=31/SCL=32），
 * 地址 0x41；同一条总线上还有触摸一体屏 ST7123(0x55)、IO 扩展器(0x43/0x44)。
 *
 * ⚠ 所以**不能**在这里自己装一套 I2C 驱动：老 API 的 i2c_driver_install 会占住
 * I2C_NUM_0，BSP 随后的 i2c_new_master_bus 建不起来 → 扩展器初始化失败 →
 * 面板/触摸停在复位（黑屏）。必须复用 BSP 的 bus handle（新 i2c_master API，
 * 总线自带仲裁与加锁，多设备共存是它的正常用法）。
 *
 * 寄存器：0xFF = 器件 ID（应读回 0x2260）；0x02 = Bus Voltage，1.25mV/LSB，
 * 报的是**2S 电包**电压（换算见 targets/tab5/config.h）。
 * 出厂 CONFIG 就是连续转换（shunt+bus），只读电压不必先写配置。 */
#ifdef ESP_PLATFORM
#include <driver/i2c_master.h>

extern esp_err_t bsp_i2c_init(void);
extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);

static i2c_master_dev_handle_t ina226_dev = NULL;
static int ina226_dbg_count = 0;

static bool rg_ina226_ensure(void)
{
    if (ina226_dev)
        return true;
    if (bsp_i2c_init() != ESP_OK)
        return false;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus)
        return false;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x41,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &ina226_dev) != ESP_OK)
    {
        ina226_dev = NULL;
        return false;
    }
    /* 器件 ID 自检：读不到就不是"没配置"，是总线/地址不对，早点暴露 */
    uint8_t id_reg = 0xFF, id[2] = {0};
    if (i2c_master_transmit_receive(ina226_dev, &id_reg, 1, id, 2, 50) != ESP_OK)
        RG_LOGE("INA226: device ID read failed (bus down?)\n");
    else
        RG_LOGI("INA226 ready: ID=0x%02X%02X (expect 2260)\n", id[0], id[1]);
    return true;
}

static bool rg_ina226_read_reg16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2] = {0};
    if (!rg_ina226_ensure())
        return false;
    if (i2c_master_transmit_receive(ina226_dev, &reg, 1, buf, 2, 50) != ESP_OK)
        return false;
    *out = (uint16_t)((buf[0] << 8) | buf[1]);
    return true;
}
/* 分流电流（mA） = 寄存器原始值 * 0.5：
 * 分流电压 LSB = 2.5µV、分流电阻 5mΩ → I = raw * 2.5µV / 5mΩ = raw * 0.5 mA。
 * （5mΩ 与官方 M5Unified 对 Tab5 的 cfg.shunt_res 一致，见 Power_Class.inl。） */
static int rg_ina226_shunt_ma(int16_t raw)
{
    return (int)(raw * 0.5f);
}

#else /* 宿主（SDL 预览）没有 BSP：不参与电量读取 */
static int ina226_dbg_count = 0;
static bool rg_ina226_read_reg16(uint8_t reg, uint16_t *out) { return false; }
#endif
#endif /* RG_BATTERY_DRIVER == 3 */

bool rg_input_read_battery_raw(rg_battery_t *out)
{
    uint32_t raw_value = 0;
    bool present = true;
    bool charging = false;

#if RG_BATTERY_DRIVER == 1 /* ADC */
    for (int i = 0; i < 4; ++i)
    {
        int value = adc_get_raw(RG_BATTERY_ADC_UNIT, RG_BATTERY_ADC_CHANNEL);
        if (value < 0)
            return false;
        raw_value += esp_adc_cal_raw_to_voltage(value, &adc_chars);
    }
    raw_value /= 4;
#elif RG_BATTERY_DRIVER == 2 /* I2C */
    uint8_t data[5];
    if (!rg_i2c_read(0x20, -1, &data, 5))
        return false;
    raw_value = data[4];
    charging = data[4] == 255;
#elif RG_BATTERY_DRIVER == 3 /* INA226 @0x41: reg 0x02 总线电压(1.25mV/LSB) + reg 0x01 分流电压(2.5µV/LSB) */
    /* Tab5 的电池电压由 INA226 给出，读法见本文件上方的 rg_ina226_*（走 BSP 主 I2C）。
     * 它报的是 2S 电包电压 → 百分比换算在 targets/tab5/config.h 里折半。 */
    uint16_t word = 0, shunt = 0;
    bool ok = rg_ina226_read_reg16(0x02, &word);
    /* 充电判定走**分流电流方向**（分流电阻 5mΩ，官方口径：充电时该值为负）。
     * 只读寄存器、不写配置：INA226 出厂 CONFIG 已是连续转换（shunt+bus），
     * 而电流方向不需要 CALIBRATION（那是给 CURRENT 寄存器用的），读 0x01 就够。 */
    bool ok_shunt = rg_ina226_read_reg16(0x01, &shunt);
    const int cur_ma = ok_shunt ? rg_ina226_shunt_ma((int16_t)shunt) : 0;
    charging = ok_shunt && (cur_ma < -RG_TAB5_CHARGE_CURRENT_MA);
    if (ina226_dbg_count < 6)
    {
        RG_LOGI("INA226-DBG: rc=%d raw=0x%04X (%d mV) shunt=%d (%d mA) chg=%d\n",
                (int)ok, (unsigned)word, ok ? (int)(word * 1.25f) : -1,
                (int)(int16_t)shunt, cur_ma, (int)charging);
        ina226_dbg_count++;
    }
    /* 原始采样的跳变各打一行 —— 这行是**未去抖的原始值**，只用于确认符号约定与抖动形态；
     * 屏上圆灯看的是 input_task 里表决后的结果（那行日志才是"灯为什么这样"的答案）。
     * ⚠ 别把这行当灯的状态读：实测它会 -871mA 与 +1mA 交替。 */
    {
        static int last_chg = -1;
        int chg = (int)charging;
        if (chg != last_chg)
        {
            RG_LOGI("INA226-CHG(raw): shunt=%d (%d mA) -> charging=%d (未去抖)\n",
                    (int)(int16_t)shunt, cur_ma, chg);
            last_chg = chg;
        }
    }
    if (!ok || word == 0 || word == 0xFFFF)
        return false;
    raw_value = (uint32_t)(word * 1.25f);   /* 寄存器原始值 → 电包 mV */
#else
    return false;
#endif

    if (!out)
        return true;

    *out = (rg_battery_t){
        .level = RG_MAX(0.f, RG_MIN(100.f, RG_BATTERY_CALC_PERCENT(raw_value))),
        .volts = RG_BATTERY_CALC_VOLTAGE(raw_value),
        .present = present,
        .charging = charging,
    };
    return true;
}

#if defined(RG_GAMEPAD_TOUCH_MAP) && defined(ESP_PLATFORM)
/* ────────────────────────────────────────────────────────────── 方向键：矢量扇区判定
 * 2026-09-29（用户拍板，方案见 tools/preview-touch-overlay.py --dpad → docs/dpad-zones.png）：
 * **十字外观一个字不改**，只把"命中"从 4 个矩形换成矢量扇区。现状的两个先天毛病：
 *   ① 四个矩形做不出斜向（一指只能压一个矩形）；② 中心有 86px 空洞 —— 手指从上滑到左
 *   要穿过它，于是"断键"，得抬手重按（这是它不如摇杆顺的主因）。
 * 规则（与 PC 参考实现同参数，改参数两处一起改）：
 *   · r < 30px   → 死区，无方向            · r > 135px → 出界，无方向（臂外沿 127 + 8）
 *   · 夹角落在 30°~60° **且** r ≥ 70px → 斜向（同时按两个方向键）
 *   · 其余       → 单轴（|dx| 与 |dy| 谁大听谁的）
 *   · 滞回：已斜向时角度带放宽 5°、半径门槛降到 60px → 手指压在边界不来回跳键
 * 注：GB/GBC/GBA 硬件本身是数字十字键，核心只吃位掩码 —— 所以这里做的是"数字 8 向"
 * （斜向 + 不断键 + 判定宽容），不是模拟量。斜向当前**没有开关**（先上真机试手感；
 * 若某个平台游戏嫌误触，再加一个 Menu 开关，一处宏就能锁死）。
 * 实现细节：判据全用整数（tan 值放大 1000 倍 + 平方距离），不引 libm、不上浮点；
 * 几何从 keymap_touch 里算（中心 = 上键与左键的交点），键位表挪了判定跟着走；
 * 那四个矩形条目**保留**（可视层仍用它们画十字与按下高亮）。 */
#define RG_DPAD_PAD_R        135    /* 可触半径（臂外沿 127 + 8 余量） */
#define RG_DPAD_DEAD_R       30     /* 中心死区半径 */
#define RG_DPAD_DIAG_R       70     /* 斜向还要求推到这么远（贴中心蹭到角不算） */
#define RG_DPAD_DIAG_R_LEAVE 60     /* 滞回：已斜向时退到这里才掉回单轴 */
#define RG_DPAD_TAN30_X1000  577    /* tan(30°) */
#define RG_DPAD_TAN60_X1000  1732   /* tan(60°) */
#define RG_DPAD_TAN25_X1000  466    /* tan(25°)：滞回后斜区的下边界 */
#define RG_DPAD_TAN65_X1000  2145   /* tan(65°)：滞回后斜区的上边界 */

static struct { int cx, cy; bool ready; } dpad_geom;
static bool dpad_was_diag;          /* 上一轮是否斜向（滞回状态） */

static void rg_dpad_geom_update(void)
{
    const rg_keymap_touch_t *up = NULL, *down = NULL, *left = NULL, *right = NULL;
    for (size_t i = 0; i < RG_COUNT(keymap_touch); ++i)
    {
        rg_key_t k = keymap_touch[i].key;
        if (k == RG_KEY_UP)         up = &keymap_touch[i];
        else if (k == RG_KEY_DOWN)  down = &keymap_touch[i];
        else if (k == RG_KEY_LEFT)  left = &keymap_touch[i];
        else if (k == RG_KEY_RIGHT) right = &keymap_touch[i];
    }
    if (!up || !down || !left || !right)
    {
        dpad_geom.ready = false;
        RG_LOGW("dpad: 键位表里没找齐 上/下/左/右，矢量判定关闭（仍按矩形命中）\n");
        return;
    }
    dpad_geom.cx = up->x;           /* 中心 = 上键的 x 与左键的 y 的交点 */
    dpad_geom.cy = left->y;
    dpad_geom.ready = true;
    RG_LOGI("dpad: 矢量扇区判定 ready（中心 %d,%d / 臂距 %d / 可触 %d / 死区 %d / 斜向门槛 %d）\n",
            dpad_geom.cx, dpad_geom.cy, up->x - left->x,
            RG_DPAD_PAD_R, RG_DPAD_DEAD_R, RG_DPAD_DIAG_R);
}

static void rg_dpad_reset(void)
{
    dpad_was_diag = false;          /* 手指离开方向键区 → 下次按下按"进入"门槛判 */
}

/* 返回该点应产生的方向键；0 = 这个点不在方向键的可触圆内（交给矩形命中逻辑） */
static uint32_t rg_dpad_keys_at(int lx, int ly)
{
    if (!dpad_geom.ready)
        return 0;
    const int dx = lx - dpad_geom.cx, dy = ly - dpad_geom.cy;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    const int r2 = ax * ax + ay * ay;
    if (r2 < RG_DPAD_DEAD_R * RG_DPAD_DEAD_R)
        return 0;                                   /* 死区（比原来的 86px 空洞小得多） */
    if (r2 > RG_DPAD_PAD_R * RG_DPAD_PAD_R)
        return 0;                                   /* 出界 */

    const int lo = dpad_was_diag ? RG_DPAD_TAN25_X1000 : RG_DPAD_TAN30_X1000;
    const int hi = dpad_was_diag ? RG_DPAD_TAN65_X1000 : RG_DPAD_TAN60_X1000;
    const int gate = dpad_was_diag ? RG_DPAD_DIAG_R_LEAVE : RG_DPAD_DIAG_R;
    const bool in_band = (ay * 1000 > ax * lo) && (ay * 1000 < ax * hi);

    uint32_t keys;
    if (in_band && r2 >= gate * gate)
        keys = (dy < 0 ? RG_KEY_UP : RG_KEY_DOWN) | (dx < 0 ? RG_KEY_LEFT : RG_KEY_RIGHT);
    else
        keys = (ax >= ay) ? (dx < 0 ? RG_KEY_LEFT : RG_KEY_RIGHT)
                          : (dy < 0 ? RG_KEY_UP : RG_KEY_DOWN);
    dpad_was_diag = (keys & (keys - 1)) != 0;       /* 同时有两位 = 斜向 */
    return keys;
}
#endif

#if defined(RG_GAMEPAD_TOUCH_MAP) && defined(ESP_PLATFORM)
/* 「X/Y ↔ L/R 调换」按钮（L/R 中间那颗）的按下状态：用于边沿检测 ——
 * 手指按着不放时每一轮读点都会命中它，不做边沿就会疯狂来回切。 */
static bool swap_btn_down = false;
#endif

bool rg_input_read_gamepad_raw(uint32_t *out)
{
    uint32_t state = 0;

#if defined(RG_GAMEPAD_ADC_MAP)
    static int old_adc_values[RG_COUNT(keymap_adc)];
    for (size_t i = 0; i < RG_COUNT(keymap_adc); ++i)
    {
        const rg_keymap_adc_t *mapping = &keymap_adc[i];
        int value = adc_get_raw(mapping->unit, mapping->channel);
        if (value >= mapping->min && value <= mapping->max)
        {
            if (abs(old_adc_values[i] - value) < RG_GAMEPAD_ADC_FILTER_WINDOW)
                state |= mapping->key;
            // else
            //     RG_LOGD("Rejected input: %d", old_adc_values[i] - value);
            old_adc_values[i] = value;
        }
    }
#endif

#if defined(RG_GAMEPAD_GPIO_MAP)
    for (size_t i = 0; i < RG_COUNT(keymap_gpio); ++i)
    {
        const rg_keymap_gpio_t *mapping = &keymap_gpio[i];
        if (gpio_get_level(mapping->num) == mapping->level)
            state |= mapping->key;
    }
#endif

#if defined(RG_GAMEPAD_I2C_MAP)
    uint32_t buttons = 0;
#if defined(RG_I2C_GPIO_DRIVER)
    int data0 = rg_i2c_gpio_read_port(0), data1 = rg_i2c_gpio_read_port(1);
    if (data0 > -1 && data1 > -1)
    {
        buttons = (data1 << 8) | (data0);
#elif defined(RG_TARGET_T_DECK_PLUS)
    uint8_t data[5];
    if (rg_i2c_read(T_DECK_KBD_ADDRESS, -1, &data, 5))
    {
        buttons = ((data[0] << 25) | (data[1] << 18) | (data[2] << 11) | ((data[3] & 0xF8) << 4) | (data[4]));
#else
    uint8_t data[5];
    if (rg_i2c_read(RG_I2C_GPIO_ADDR, -1, &data, 5))
    {
        buttons = (data[2] << 8) | (data[1]);
#endif
        for (size_t i = 0; i < RG_COUNT(keymap_i2c); ++i)
        {
            const rg_keymap_i2c_t *mapping = &keymap_i2c[i];
            if (((buttons >> mapping->num) & 1) == mapping->level)
                state |= mapping->key;
        }
    }
#endif

#if defined(RG_GAMEPAD_KBD_MAP)
#ifdef RG_TARGET_SDL2
    int numkeys = 0;
    const uint8_t *keys = SDL_GetKeyboardState(&numkeys);
    for (size_t i = 0; i < RG_COUNT(keymap_kbd); ++i)
    {
        const rg_keymap_kbd_t *mapping = &keymap_kbd[i];
        if (mapping->src < 0 || mapping->src >= numkeys)
            continue;
        if (keys[mapping->src])
            state |= mapping->key;
    }
#else
#warning "not implemented"
#endif
#endif

#if defined(RG_GAMEPAD_TOUCH_MAP) && defined(ESP_PLATFORM)
    /* 本轮有没有读到真实触点。**必须在读函数外面判**（见函数末尾的复位块）：
     * IDF 的 esp_lcd_touch_get_coordinates() 在无触点时返回 false
     * （它直接把驱动 get_xy 的返回值当自己的返回值，ST7123 无触点就是 false），
     * 于是"手指抬起"这一拍整个分支都被跳过 —— 任何写在分支里的"抬指复位"
     * 都永远跑不到。2026-09-29 真机反馈"再按切换键换不回来"的根因就在这里。 */
    bool any_touch = false;
    if (rg_touch_ensure() && esp_lcd_touch_read_data(touch_handle) == ESP_OK)
    {
        uint16_t px[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
        uint16_t py[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
        uint8_t count = 0;
        if (esp_lcd_touch_get_coordinates(touch_handle, px, py, NULL, &count, CONFIG_ESP_LCD_TOUCH_MAX_POINTS))
        {
            any_touch = (count > 0);
#if RG_TOUCH_OVERLAY
            /* "看得见"和"点得到"必须同源，两个条件都要满足：
             *   ① 用户没关掉按键（visible）—— 否则黑边上一按就触发，用户以为触摸坏了；
             *   ② 建层成功（is_ready）—— 分配失败时按键根本没画出来，同样不能命中。
             * 只判 visible 会漏掉 ②：那时屏幕上看不到任何按键，命中却照旧生效。 */
            const bool pad_visible = rg_overlay_get_visible() && rg_overlay_is_ready();
#else
            const bool pad_visible = true;
#endif
            bool dpad_touched = false;
            for (int t = 0; t < count && pad_visible; ++t)
            {
                int lx = 0, ly = 0;
                RG_TOUCH_LOGICAL_FROM_PHYS((int)px[t], (int)py[t], lx, ly);
                /* 方向键先走**矢量扇区判定**（2026-09-29）：落在可触圆内就直接出结果
                 * （十字外观没变，换的是判定）；圆外才继续按矩形命中。
                 * 圆内不会误伤别的键：可触圆半径 135 只覆盖十字本身（最近的 L 键边沿
                 * 在 y=587，圆心 y=780 - 135 = 645，够不着）。 */
                const uint32_t dpad = rg_dpad_keys_at(lx, ly);
                if (dpad)
                {
                    state |= dpad;
                    dpad_touched = true;
                    continue;
                }
                for (size_t i = 0; i < RG_COUNT(keymap_touch); ++i)
                {
                    const rg_keymap_touch_t *mapping = &keymap_touch[i];
                    /* 半开区间 [x-w/2, x+w/2)：右/下侧用 `<` 而不是 `<=`。
                     * 闭区间下、相邻两个命中区共享边界像素时会同时触发两颗键；
                     * 当前键位表处处留了 ≥1px 间隙（属理论风险），改成半开区间后
                     * 就不再依赖"表里必须留缝"这个隐含前提（走查 P2-13）。 */
                    if (lx >= mapping->x - mapping->w / 2 && lx < mapping->x + mapping->w / 2 &&
                        ly >= mapping->y - mapping->h / 2 && ly < mapping->y + mapping->h / 2)
                        /* 调换开着时这个**位置**代表的是别的键（X↔R、Y↔L）——
                         * **必须调取叠加层的同一个函数**（rg_overlay_map_key，它内部按状态门控）：
                         * 规则（rg_touch_swap_key，自反）与状态（swap_yx）都只此一处，
                         * 可视层的键位表/标签/配色读的是同一个答案，所以按下高亮与手感永远一致。
                         * ⚠ 别在这里自己写 `swap ? swap_key(k) : k`：swap_key 是自反对换不是恒等，
                         * 漏掉状态门控就会永远停在"已调换"那一侧 ——
                         * 真机 2026-09-29：默认 X/Y 模式下按 X 亮的是 R、按 Y 亮的是 L。 */
                        state |= rg_overlay_map_key(mapping->key);
                }
            }

            /* 手指离开方向键区（或整体抬指）→ 清掉滞回状态，下次按下按"进入"门槛判 */
            if (!dpad_touched)
                rg_dpad_reset();

#if RG_TOUCH_OVERLAY
            /* ── X/Y ↔ L/R 调换按钮（L/R 中间那颗）─────────────────────────────
             * 不是游戏按键：不注入任何 RG_KEY_*，只切映射。边沿检测见 swap_btn_down
             * 的注释（按着不放不能反复切）。几何来自可视层（单一数据源）。 */
            if (pad_visible)
            {
                int sx = 0, sy = 0, sw = 0, sh = 0;
                rg_overlay_get_swap_rect(&sx, &sy, &sw, &sh);
                bool hit = false;
                for (int t = 0; t < count; ++t)
                {
                    int lx = 0, ly = 0;
                    RG_TOUCH_LOGICAL_FROM_PHYS((int)px[t], (int)py[t], lx, ly);
                    if (lx >= sx && lx < sx + sw && ly >= sy && ly < sy + sh)
                    {
                        hit = true;
                        break;
                    }
                }
                if (hit && !swap_btn_down)
                {
                    const bool on = !rg_overlay_get_swap();
                    rg_overlay_set_swap(on);
                    RG_LOGI("touch swap: X/Y <-> L/R -> %s\n", on ? "L/R" : "X/Y");
                }
                swap_btn_down = hit;
            }
            else
            {
                swap_btn_down = false;
            }
#endif

            /* ── 恢复入口：点左上角那个可见开关 ────────────────────────────────
             * 按键隐藏时可视层会在左上角画一个十字键图标的开关（rg_touch_overlay.c），
             * 这里只负责命中它 → 把按键放回来（并立即落盘，断电也不丢）。
             * 开关本身不是按键：不注入任何 RG_KEY_*，只切 visible。 */
            if (!pad_visible && count > 0)
            {
                int tx = 0, ty = 0, tw = 0, th = 0;
                rg_overlay_get_toggle_rect(&tx, &ty, &tw, &th);
                /* 遍历所有触点，不只 px[0]：ST7123 多点上报里第 0 点可能是上一次的
                 * 残留坐标（按键命中之所以一直正常，就是因为它遍历了全部点）。 */
                for (int t = 0; t < count; ++t)
                {
                    int lx = 0, ly = 0;
                    RG_TOUCH_LOGICAL_FROM_PHYS((int)px[t], (int)py[t], lx, ly);
                    if (lx >= tx && lx < tx + tw && ly >= ty && ly < ty + th)
                    {
                        RG_LOGI("touch overlay: toggle tapped, showing buttons\n");
                        rg_overlay_init();          /* 若之前建层失败，这里补一次 */
                        rg_overlay_set_visible(true);
                        break;
                    }
                }
            }
        }
    }

    /* 抬指 / 这一拍没有任何触点 → 清掉"按住"类状态。
     * ⚠ 位置很关键：必须在上面两层 `if`（read_data / get_coordinates）**外面**。
     * 无触点时 get_coordinates 返回 false、分支整块被跳过，写在里面的复位永远不执行 ——
     * 边沿检测会一直以为手指还按着，于是"调换"只能切一次（真机："再按换不回来"）。
     * 方向键的滞回状态同理（它写在分支里，注释说"或整体抬指"其实抬指那一拍看不到）。 */
    if (!any_touch)
    {
        if (swap_btn_down)
            RG_LOGI("touch swap: released\n");
        swap_btn_down = false;
        rg_dpad_reset();
    }
#endif

#if defined(RG_GAMEPAD_SERIAL_MAP)
    gpio_set_level(RG_GPIO_GAMEPAD_LATCH, 0);
    rg_usleep(5);
    gpio_set_level(RG_GPIO_GAMEPAD_LATCH, 1);
    rg_usleep(1);
    uint32_t buttons = 0;
    for (int i = 0; i < 16; i++)
    {
        buttons |= gpio_get_level(RG_GPIO_GAMEPAD_DATA) << (15 - i);
        gpio_set_level(RG_GPIO_GAMEPAD_CLOCK, 0);
        rg_usleep(1);
        gpio_set_level(RG_GPIO_GAMEPAD_CLOCK, 1);
        rg_usleep(1);
    }
    for (size_t i = 0; i < RG_COUNT(keymap_serial); ++i)
    {
        const rg_keymap_serial_t *mapping = &keymap_serial[i];
        if (((buttons >> mapping->num) & 1) == mapping->level)
            state |= mapping->key;
    }
#endif

#if defined(RG_GAMEPAD_VIRT_MAP)
    for (size_t i = 0; i < RG_COUNT(keymap_virt); ++i)
    {
        if (state == keymap_virt[i].src)
            state = keymap_virt[i].key;
    }
#endif

    if (out)
        *out = state;
    return true;
}

static void input_task(void *arg)
{
    uint8_t debounce[RG_KEY_COUNT];
    uint32_t local_gamepad_state = 0;
    uint32_t state;
    int64_t next_battery_update = 0;

    // Start the task with debounce history full to allow a button held during boot to be detected
    memset(debounce, 0xFF, sizeof(debounce));
    input_task_running = true;
    input_task_exited = false;

    while (input_task_running)
    {
        if (rg_input_read_gamepad_raw(&state))
        {
            for (int i = 0; i < RG_KEY_COUNT; ++i)
            {
                uint32_t val = ((debounce[i] << 1) | ((state >> i) & 1));
                debounce[i] = val & 0xFF;

                if ((val & ((1 << RG_GAMEPAD_DEBOUNCE_PRESS) - 1)) == ((1 << RG_GAMEPAD_DEBOUNCE_PRESS) - 1))
                {
                    local_gamepad_state |= (1 << i); // Pressed
                }
                else if ((val & ((1 << RG_GAMEPAD_DEBOUNCE_RELEASE) - 1)) == 0)
                {
                    local_gamepad_state &= ~(1 << i); // Released
                }
            }
            gamepad_state = local_gamepad_state;
        }

        if (rg_system_timer() >= next_battery_update)
        {
            rg_battery_t temp = {0};
            if (rg_input_read_battery_raw(&temp))
            {
                if (fabsf(battery_state.level - temp.level) < RG_BATTERY_UPDATE_THRESHOLD)
                    temp.level = battery_state.level;
                if (fabsf(battery_state.volts - temp.volts) < RG_BATTERY_UPDATE_THRESHOLD_VOLT)
                    temp.volts = battery_state.volts;
            }
            battery_state = temp;
            /* 【充电标志表决 · 2026-09-29 重做，走查 P2-14】
             * 背景：INA226 的分流读值会抖（实测 -871mA 与 +1mA 交替采样），单次采样直接
             * 翻转 charging 会让圆灯"亮一下灭一下"，很不自然（v0.4.1 已先加过一版去抖）。
             *
             * 现在改成"最近 5 次采样的窗口表决"，判据刻意**不对称**：
             *   ≥3 次为充电 → 认定为充电     （充电=负电流是特异性信号，宁可早点亮）
             *     0 次为充电 → 认定为未充电   （要求连续 5 次都没充上，抗抖动）
             *   1~2 次        → 灰色地带，**保持现状不动**
             * 为什么这样选：交替抖动下窗口里命中次数只会在 2~3 之间晃，既到不了 0（不会
             * 误灭）也很快能凑到 3（会正确点亮）——原来那版"连续两次反向就翻转"在真实
             * 噪声里只要出现连续两次同向就会翻一次，表现为偶发单闪。
             * 延迟：插线 ≈3 个周期（6s）内点亮；拔线 ≈5 个周期（10s）内熄灭。周期是
             * 下面 next_battery_update 的 2s。首次采样会把窗口**铺满**（否则开机会白等 10s）。 */
            {
                static uint8_t hist;        /* bit0 = 最新一次采样，1 = 充电 */
                static int samples;
                if (samples == 0)
                    hist = battery_state.charging ? 0x1F : 0x00;   /* 首次：整窗铺满，立刻生效 */
                hist = (uint8_t)(((hist << 1) | (battery_state.charging ? 1 : 0)) & 0x1F);
                if (samples < 5)
                    samples++;
                const int ones = __builtin_popcount((unsigned)hist);
                if (ones >= 3)
                    battery_state.charging = true;
                else if (ones == 0)
                    battery_state.charging = false;
                /* 1~2：灰色地带，保持上一次的表决结果 */
                if (samples <= 2 || (battery_state.charging != battery_state_prev_charging))
                {
                    battery_state_prev_charging = battery_state.charging ? 1 : 0;
                    /* 表决结果变化的这行日志才是"屏上圆灯为什么这样"的答案
                     * （INA226-CHG(raw) 那行是未去抖的原始值，别拿来对照灯）。 */
                    RG_LOGI("battery: charging=%d (窗口 5 取≥3 表决, raw=%d)\n",
                            (int)battery_state.charging, (int)temp.charging);
                }
            }
            next_battery_update = rg_system_timer() + 2 * 1000000; // update every 2 seconds
        }

        rg_task_delay(10);
    }

    input_task_running = false;
    gamepad_state = -1;
    input_task_exited = true;
}

void rg_input_init(void)
{
    RG_ASSERT(!input_task_running, "Input already initialized!");

#if defined(RG_GAMEPAD_ADC_MAP)
    RG_LOGI("Initializing ADC gamepad driver...");
    adc1_config_width(ADC_WIDTH_MAX - 1);
    for (size_t i = 0; i < RG_COUNT(keymap_adc); ++i)
    {
        const rg_keymap_adc_t *mapping = &keymap_adc[i];
        if (mapping->unit == ADC_UNIT_1)
            adc1_config_channel_atten(mapping->channel, mapping->atten);
        else if (mapping->unit == ADC_UNIT_2)
            adc2_config_channel_atten(mapping->channel, mapping->atten);
        else
            RG_LOGE("Invalid ADC unit %d!", (int)mapping->unit);
    }
    UPDATE_GLOBAL_MAP(keymap_adc);
#endif

#if defined(RG_GAMEPAD_GPIO_MAP)
    RG_LOGI("Initializing GPIO gamepad driver...");
    for (size_t i = 0; i < RG_COUNT(keymap_gpio); ++i)
    {
        const rg_keymap_gpio_t *mapping = &keymap_gpio[i];
        gpio_set_direction(mapping->num, GPIO_MODE_INPUT);
        if (mapping->pullup && mapping->pulldown)
            gpio_set_pull_mode(mapping->num, GPIO_PULLUP_PULLDOWN);
        else if (mapping->pullup || mapping->pulldown)
            gpio_set_pull_mode(mapping->num, mapping->pullup ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
        else
            gpio_set_pull_mode(mapping->num, GPIO_FLOATING);
    }
    UPDATE_GLOBAL_MAP(keymap_gpio);
#endif

#if defined(RG_GAMEPAD_I2C_MAP)
    RG_LOGI("Initializing I2C gamepad driver...");
    rg_i2c_init();
#if defined(RG_I2C_GPIO_DRIVER)
    for (size_t i = 0; i < RG_COUNT(keymap_i2c); ++i)
    {
        const rg_keymap_i2c_t *mapping = &keymap_i2c[i];
        if (mapping->pullup)
            rg_i2c_gpio_set_direction(mapping->num, RG_GPIO_INPUT_PULLUP);
        else
            rg_i2c_gpio_set_direction(mapping->num, RG_GPIO_INPUT);
    }
#elif defined(RG_TARGET_T_DECK_PLUS)
    rg_i2c_write_byte(T_DECK_KBD_ADDRESS, -1, T_DECK_KBD_MODE_RAW_CMD);
#endif
    UPDATE_GLOBAL_MAP(keymap_i2c);
#endif

#if defined(RG_GAMEPAD_KBD_MAP)
    RG_LOGI("Initializing KBD gamepad driver...");
    UPDATE_GLOBAL_MAP(keymap_kbd);
#endif

#if defined(RG_GAMEPAD_SERIAL_MAP)
    RG_LOGI("Initializing SERIAL gamepad driver...");
    gpio_set_direction(RG_GPIO_GAMEPAD_CLOCK, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_GAMEPAD_LATCH, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_GAMEPAD_DATA, GPIO_MODE_INPUT);
    gpio_set_level(RG_GPIO_GAMEPAD_LATCH, 0);
    gpio_set_level(RG_GPIO_GAMEPAD_CLOCK, 1);
    UPDATE_GLOBAL_MAP(keymap_serial);
#endif


#if RG_BATTERY_DRIVER == 1 /* ADC */
    RG_LOGI("Initializing ADC battery driver...");
    if (RG_BATTERY_ADC_UNIT == ADC_UNIT_1)
    {
        adc1_config_width(ADC_WIDTH_MAX - 1); // there is no adc2_config_width
        adc1_config_channel_atten(RG_BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_11);
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_MAX - 1, 1100, &adc_chars);
    }
    else if (RG_BATTERY_ADC_UNIT == ADC_UNIT_2)
    {
        adc2_config_channel_atten(RG_BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_11);
        esp_adc_cal_characterize(ADC_UNIT_2, ADC_ATTEN_DB_11, ADC_WIDTH_MAX - 1, 1100, &adc_chars);
    }
    else
    {
        RG_LOGE("Only ADC1 and ADC2 are supported for ADC battery driver!");
    }
#endif

    // The first read returns bogus data in some drivers, waste it.
#if defined(RG_GAMEPAD_TOUCH_MAP)
    /* 只登记"这些虚拟按键存在"，真正的触摸 IC 创建推迟到第一次读手柄时（懒加载）：
     * 此刻 TP_RST 还没释放（扩展器在显示初始化里才初始化），现在创建必然失败。 */
    RG_LOGI("Virtual touch gamepad registered (ST7123 @0x55, lazy init).\n");
    UPDATE_GLOBAL_MAP(keymap_touch);
#endif
#if defined(RG_GAMEPAD_TOUCH_MAP) && defined(ESP_PLATFORM)
    /* 方向键几何（中心/臂距）从键位表算一次，矢量扇区判定要用 */
    rg_dpad_geom_update();
#endif

    rg_input_read_gamepad_raw(NULL);

    // Start background polling
    rg_task_create("rg_input", &input_task, NULL, 3 * 1024, RG_TASK_PRIORITY_6, 1);
    while (gamepad_state == -1)
        rg_task_yield();
    RG_LOGI("Input ready. state=" PRINTF_BINARY_16 "\n", PRINTF_BINVAL_16(gamepad_state));
}

void rg_input_deinit(void)
{
    input_task_running = false;
    /* 先等输入任务真正退出（走查 P2-6）：老代码把这段等待注释掉了，于是下面几个释放动作
     * 可能与"正在跑的那一轮"撞车 —— touch_handle 置 NULL 之后 rg_touch_ensure() 还会因为
     * attempts<20 重新申请一个（在正要被拆除的 I2C 总线上），而且那个新句柄没人释放。
     * 上限 300ms：正常一轮循环 ≤10ms，绝不为了等它把关机卡住。 */
    for (int i = 0; i < 30 && !input_task_exited; ++i)
        rg_task_delay(10);
#if defined(RG_GAMEPAD_TOUCH_MAP) && defined(ESP_PLATFORM)
    /* 关掉懒加载重试通道：deinit 之后任何一次 rg_touch_ensure() 都必须直接失败 */
    touch_init_attempts = RG_TOUCH_MAX_ATTEMPTS;
    if (touch_handle)
    {
        esp_lcd_touch_del(touch_handle);
        touch_handle = NULL;
    }
#endif
#if defined(ESP_PLATFORM) && RG_BATTERY_DRIVER == 3
    /* INA226 设备句柄归还总线（走查 P2-7）：不加这段，同一进程里反复进出游戏会一直累积 */
    if (ina226_dev)
    {
        i2c_master_bus_rm_device(ina226_dev);
        ina226_dev = NULL;
    }
#endif
    RG_LOGI("Input terminated.\n");
}

uint32_t rg_input_read_gamepad(void)
{
#ifdef RG_TARGET_SDL2
    /* macOS：只有主线程能泵 Cocoa 事件（子线程会触发 NSApplication 异常 → abort） */
    if (rg_sdl2_can_pump_events())
    {
        SDL_PumpEvents();
        /* 处理退出请求：关窗口 / Ctrl-C / SIGTERM 都会被 SDL 转成 SDL_QUIT 事件。
         * SDL 自带 SIGTERM/SIGINT 处理器，只把事件塞进队列 —— 不取出来进程就永远杀不掉。 */
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
                exit(0);
        }
    }
#endif
    return gamepad_state;
}

bool rg_input_key_is_pressed(rg_key_t mask)
{
    return (bool)(rg_input_read_gamepad() & mask);
}

bool rg_input_wait_for_key(rg_key_t mask, bool pressed, int timeout_ms)
{
    int64_t expiration = timeout_ms < 0 ? INT64_MAX : (rg_system_timer() + timeout_ms * 1000);
    while (rg_input_key_is_pressed(mask) != pressed)
    {
        if (rg_system_timer() > expiration)
            return false;
        rg_task_delay(10);
    }
    return true;
}

rg_battery_t rg_input_read_battery(void)
{
    return battery_state;
}

const char *rg_input_get_key_name(rg_key_t key)
{
    switch (key)
    {
    case RG_KEY_UP: return "Up";
    case RG_KEY_RIGHT: return "Right";
    case RG_KEY_DOWN: return "Down";
    case RG_KEY_LEFT: return "Left";
    case RG_KEY_SELECT: return "Select";
    case RG_KEY_START: return "Start";
    case RG_KEY_MENU: return "Menu";
    case RG_KEY_OPTION: return "Option";
    case RG_KEY_A: return "A";
    case RG_KEY_B: return "B";
    case RG_KEY_X: return "X";
    case RG_KEY_Y: return "Y";
    case RG_KEY_L: return "Left Shoulder";
    case RG_KEY_R: return "Right Shoulder";
    case RG_KEY_NONE: return "None";
    default: return "Unknown";
    }
}

const char *rg_input_get_key_mapping(rg_key_t key)
{
    if ((gamepad_mapped & key) == key)
        return "PHYSICAL";
    return NULL;
}

const rg_keyboard_layout_t virtual_map1 = {
    .layout = "0123456789"
              "ABCDEFGHIJ"
              "KLMNOPQRST"
              "UVWXYZ ,. ",
    .columns = 10,
    .rows = 4,
};

int rg_input_read_keyboard(const rg_keyboard_layout_t *map)
{
    int cursor = -1;
    int count = map->columns * map->rows;

    if (!map)
        map = &virtual_map1;

    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);

    while (1)
    {
        uint32_t joystick = rg_input_read_gamepad();
        int prev_cursor = cursor;

        if (joystick & RG_KEY_A)
            return map->layout[cursor];
        if (joystick & RG_KEY_B)
            break;

        if (joystick & RG_KEY_LEFT)
            cursor--;
        if (joystick & RG_KEY_RIGHT)
            cursor++;
        if (joystick & RG_KEY_UP)
            cursor -= map->columns;
        if (joystick & RG_KEY_DOWN)
            cursor += map->columns;

        if (cursor > count - 1)
            cursor = prev_cursor;
        else if (cursor < 0)
            cursor = prev_cursor;

        cursor = RG_MIN(RG_MAX(cursor, 0), count - 1);

        if (cursor != prev_cursor)
            rg_gui_draw_keyboard(map, cursor);

        rg_input_wait_for_key(RG_KEY_ALL, false, 500);
        rg_input_wait_for_key(RG_KEY_ANY, true, 500);

        rg_system_tick(0);
    }

    return -1;
}
