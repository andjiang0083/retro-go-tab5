/*
 * Stub ILI9881C driver — not used on ST7123-based Tab5 units.
 * Provides minimal symbols so the BSP compiles.
 */
#pragma once

#include "esp_lcd_types.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_LCD_ST7121_VER_MAJOR (1)
#define ESP_LCD_ST7121_VER_MINOR (0)
#define ESP_LCD_ST7121_VER_PATCH (0)

// Minimal types for ili9881_init_data.c compatibility
typedef struct {
    uint8_t cmd;
    uint8_t *data;
    uint8_t len;
    uint32_t delay_ms;
} ili9881c_lcd_init_cmd_t;

typedef struct {
    const void *init_cmds;
    size_t init_cmds_size;
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;
        esp_lcd_dpi_panel_config_t *dpi_config;
        int lane_num;
    } mipi_config;
} ili9881c_vendor_config_t;

esp_err_t esp_lcd_new_panel_ili9881c(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
