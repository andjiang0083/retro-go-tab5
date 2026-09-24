/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief BSP LCD
 *
 * This file offers API for basic LCD control.
 * It is useful for users who want to use the LCD without the default Graphical Library LVGL.
 *
 * For standard LCD initialization with LVGL graphical library, you can call all-in-one function bsp_display_start().
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_lcd_types.h"
#include "esp_lcd_mipi_dsi.h"
#include "sdkconfig.h"

/* LCD color formats */
#define ESP_LCD_COLOR_FORMAT_RGB565 (1)
#define ESP_LCD_COLOR_FORMAT_RGB888 (2)

/* LCD display color format */
// #if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
// #define BSP_LCD_COLOR_FORMAT        (ESP_LCD_COLOR_FORMAT_RGB888)
// #else
#define BSP_LCD_COLOR_FORMAT (ESP_LCD_COLOR_FORMAT_RGB565)
//#endif
/* LCD display color bytes endianess */
#define BSP_LCD_BIGENDIAN (0)
/* LCD display color bits */
#define BSP_LCD_BITS_PER_PIXEL (16)
/* LCD display color space */
#define BSP_LCD_COLOR_SPACE (ESP_LCD_COLOR_SPACE_RGB)

/* LCD display definition 720x1280 */
/* ===== LOCAL PATCH (retro-go-tab5) =====
 * Tab5 面板原生扫描是 720x1280（竖屏）。上游 BSP 只提供"竖屏 FB + LVGL 软件旋转"
 * （bsp_display_start() 里写死 sw_rotate = true），而模拟器/retro-go 需要 1280x720
 * 横向 FB —— 每帧软件旋转 92 万像素对 GBA 帧率不划算，所以在 DPI 时序层面切成横向
 * （像素总数不变：720*1280 == 1280*720），再由驱动发 MADCTL 的 MV 位让面板把横向
 * 像素流映射到玻璃上。置 0 即完全回到上游原状。
 * 触摸 IC 的坐标系仍是竖屏原生，故 bsp_touch_new() 里单独写死 720/1280（见该处注释）。
 */
#ifndef BSP_LCD_LANDSCAPE
#define BSP_LCD_LANDSCAPE 0   /* 实测：把 DPI 流切成横向(1280x720)+MADCTL MV 会让 ST7123 黑屏(背光亮无画面)。
                               * 两家官方代码(M5 BSP / M5GFX)都用 720x1280 竖屏原生时序 + 上层旋转，故跟随。
                               * 旋转改到我们的显示层做（P3 用 PPA 硬件 SRM，见 PPA_SRM_ROTATION_ANGLE_90）。 */
#endif

#if BSP_LCD_LANDSCAPE
#define BSP_LCD_H_RES (1280)
#define BSP_LCD_V_RES (720)
#else
#define BSP_LCD_H_RES (720)
#define BSP_LCD_V_RES (1280)
#endif

#define BSP_LCD_MIPI_DSI_LCD_HSYNC (10)
#define BSP_LCD_MIPI_DSI_LCD_HBP   (40)
#define BSP_LCD_MIPI_DSI_LCD_HFP   (40)
#define BSP_LCD_MIPI_DSI_LCD_VSYNC (4)
#define BSP_LCD_MIPI_DSI_LCD_VBP   (16)
#define BSP_LCD_MIPI_DSI_LCD_VFP   (16)

#define BSP_LCD_MIPI_DSI_LANE_NUM          (2)    // 2 data lanes
#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS (730)  // 720*1280 RGB24 60Hz //(900) // 900Mbps

#define BSP_MIPI_DSI_PHY_PWR_LDO_CHAN       (3)  // LDO_VO3 is connected to VDD_MIPI_DPHY
#define BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV (2500)

/**
 * @brief BSP display configuration structure
 *
 */
typedef struct {
    int dummy;
} bsp_display_config_t;

/**
 * @brief BSP display return handles
 *
 */
typedef struct {
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus; /*!< MIPI DSI bus handle */
    esp_lcd_panel_io_handle_t io;          /*!< ESP LCD IO handle */
    esp_lcd_panel_handle_t panel;          /*!< ESP LCD panel (color) handle */
    esp_lcd_panel_handle_t control;        /*!< ESP LCD panel (control) handle */
} bsp_lcd_handles_t;

/**
 * @brief Create new display panel
 *
 * For maximum flexibility, this function performs only reset and initialization of the display.
 * You must turn on the display explicitly by calling esp_lcd_panel_disp_on_off().
 * The display's backlight is not turned on either. You can use bsp_display_backlight_on/off(),
 * bsp_display_brightness_set() (on supported boards) or implement your own backlight control.
 *
 * If you want to free resources allocated by this function, you can use esp_lcd API, ie.:
 *
 * \code{.c}
 * esp_lcd_panel_del(panel);
 * esp_lcd_panel_io_del(io);
 * esp_lcd_del_dsi_bus(mipi_dsi_bus);
 * \endcode
 *
 * @param[in]  config    display configuration
 * @param[out] ret_panel esp_lcd panel handle
 * @param[out] ret_io    esp_lcd IO handle
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd failure
 */
esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io);

/**
 * @brief Create new display panel
 *
 * For maximum flexibility, this function performs only reset and initialization of the display.
 * You must turn on the display explicitly by calling esp_lcd_panel_disp_on_off().
 * The display's backlight is not turned on either. You can use bsp_display_backlight_on/off(),
 * bsp_display_brightness_set() (on supported boards) or implement your own backlight control.
 *
 * If you want to free resources allocated by this function, you can use esp_lcd API, ie.:
 *
 * \code{.c}
 * esp_lcd_panel_del(panel);
 * esp_lcd_panel_del(control);
 * esp_lcd_panel_io_del(io);
 * esp_lcd_del_dsi_bus(mipi_dsi_bus);
 * \endcode
 *
 * @param[in]  config    display configuration
 * @param[out] ret_handles all esp_lcd handles in one structure
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd failure
 */
esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles);

/**
 * @brief Initialize display's brightness
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_brightness_init(void);

/**
 * @brief Set display's brightness
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 * Brightness must be already initialized by calling bsp_display_brightness_init() or bsp_display_new()
 *
 * @param[in] brightness_percent Brightness in [%]
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_brightness_set(int brightness_percent);

/**
 * @brief Turn on display backlight
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 * Brightness must be already initialized by calling bsp_display_brightness_init() or bsp_display_new()
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_backlight_on(void);

/**
 * @brief Turn off display backlight
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 * Brightness must be already initialized by calling bsp_display_brightness_init() or bsp_display_new()
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_backlight_off(void);

#ifdef __cplusplus
}
#endif
