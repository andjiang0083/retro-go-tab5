#include "rg_system.h"
#include "rg_input.h"

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
#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_st7123.h"
#ifndef CONFIG_ESP_LCD_TOUCH_MAX_POINTS
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
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

/* 把键位表暴露给显示层（可视层用） */
const rg_keymap_touch_t *rg_input_get_touch_keymap(size_t *count)
{
    if (count)
        *count = RG_COUNT(keymap_touch);
    return keymap_touch;
}

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
#endif
static bool input_task_running = false;
static uint32_t gamepad_state = -1; // _Atomic
static uint32_t gamepad_mapped = 0;
static rg_battery_t battery_state = {0};

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

#if defined(RG_GAMEPAD_TOUCH_MAP)
    if (rg_touch_ensure() && esp_lcd_touch_read_data(touch_handle) == ESP_OK)
    {
        uint16_t px[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
        uint16_t py[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
        uint8_t count = 0;
        if (esp_lcd_touch_get_coordinates(touch_handle, px, py, NULL, &count, CONFIG_ESP_LCD_TOUCH_MAX_POINTS))
        {
            for (int t = 0; t < count; ++t)
            {
                int lx = 0, ly = 0;
                RG_TOUCH_LOGICAL_FROM_PHYS((int)px[t], (int)py[t], lx, ly);
                for (size_t i = 0; i < RG_COUNT(keymap_touch); ++i)
                {
                    const rg_keymap_touch_t *mapping = &keymap_touch[i];
                    if (lx >= mapping->x - mapping->w / 2 && lx <= mapping->x + mapping->w / 2 &&
                        ly >= mapping->y - mapping->h / 2 && ly <= mapping->y + mapping->h / 2)
                        state |= mapping->key;
                }
            }
        }
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
            next_battery_update = rg_system_timer() + 2 * 1000000; // update every 2 seconds
        }

        rg_task_delay(10);
    }

    input_task_running = false;
    gamepad_state = -1;
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
#if defined(RG_GAMEPAD_TOUCH_MAP)
    if (touch_handle)
    {
        esp_lcd_touch_del(touch_handle);
        touch_handle = NULL;
    }
#endif
    // while (gamepad_state != -1)
    //     rg_task_yield();
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
