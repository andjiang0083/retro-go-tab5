/* ============================================================================
 * Tab5 电源/复位外围统一初始化 —— **两个显示驱动共用一份**
 * ----------------------------------------------------------------------------
 * 为什么单独抽出来：本项目同时存在两份显示驱动（`mipi_dsi_tab5.h` 横屏版、
 * `mipi_dsi_tab5_p.h` 竖屏版；本分支实际用竖屏版）。这段逻辑原先只写在竖屏版里，
 * 结果 "充电使能" 的修复只落了竖屏一份 —— 谁切回横屏构建，插 USB-C 不充电的
 * bug 就会原样复发，而且日志里看不出来（症状与踩坑时完全一样）。
 * 处置见代码走查报告 P1-1：抽成共用函数，从根上消灭"改一份漏一份"。
 *   docs/CODE-REVIEW-v0.4.1.md
 *
 * 内容（顺序不能动）：
 *   1) BSP I2C —— 扩展器 / 触摸 IC(0x55) / INA226(0x41) 都挂这条总线
 *   2) IO 扩展器 PI4IOE5V6416 —— LCD_RST(P4)/TP_RST(P5) 由它驱动，而 BSP 的
 *      `bsp_display_new_*` 自己不调它（M5 官方例程在 app_main 里调）。不调它：
 *      扩展器上电默认输出寄存器全 0 → LCD_RST=0/TP_RST=0 → 触摸 IC 不应答
 *      （日志："No known touch controller detected, defaulting to ILI9881C"）
 *      且面板被按在复位里 → 屏幕全黑。扩展器是独立芯片、寄存器状态跨 ESP 复位
 *      保留，所以"真断电"后必须由固件重新初始化它。
 *   3) 充电使能 —— BSP 的扩展器初始化把 P7(CHG_EN) 留成**低电平**（它上一行注释
 *      与被注释掉的 `0b10001001` 才是带 P7 的版本），于是 IP2326 一直被禁用：
 *      插 USB-C 不充电、INA226 分流恒为 0/放电方向、`battery_state.charging` 永远 false。
 *      官方 demo 在同一位置显式补了两句
 *      （M5Tab5-UserDemo/platforms/tab5/main/hal/hal_esp32.cpp:61），我们照抄其口径，中间 50ms 也是官方的。
 *
 * 返回值：false = BSP I2C 没起来（面板/触摸会停在复位里，调用方必须告警）。
 * 必须在 `bsp_display_new_*` **之前**调用（否则屏型探测拿不到 ST7123 会走错分支）。
 * ==========================================================================*/
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rg_system.h"          /* RG_LOGI/RG_LOGE */

/* BSP 里这几个函数的声明被关在头文件的 LVGL 段内（BSP_CONFIG_NO_GRAPHIC_LIB=1
 * 把它们编译掉了），所以照 snowveil 的做法手写 extern。 */
extern esp_err_t bsp_i2c_init(void);
extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);
extern void bsp_io_expander_pi4ioe_init(i2c_master_bus_handle_t bus_handle);
extern void bsp_set_charge_qc_en(bool en);
extern void bsp_set_charge_en(bool en);

static inline bool tab5_power_init(void)
{
    if (bsp_i2c_init() != ESP_OK)
    {
        RG_LOGE("bsp_i2c_init failed, expander NOT initialized (panel/touch stay in reset)\n");
        return false;
    }
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    RG_LOGI("PI4IOE expander init: LCD_RST/TP_RST released\n");
    /* 复位释放后给触摸 IC 一点时间再探测：否则紧随其后的屏型探测会探不到 ST7123
     * （BSP 探测只区分 ST7121/ST7123，探不到会退回 ILI9881C 分支）。 */
    vTaskDelay(pdMS_TO_TICKS(150));

    bsp_set_charge_qc_en(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_set_charge_en(true);
    RG_LOGI("charging enabled: CHG_QC=on, CHG_EN(P7)=high\n");
    return true;
}
