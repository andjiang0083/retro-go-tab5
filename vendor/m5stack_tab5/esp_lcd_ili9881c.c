/*
 * Stub ILI9881C implementation — returns error.
 * This display type is not present on ST7123-based Tab5 units.
 */
#include "esp_lcd_ili9881c.h"

esp_err_t esp_lcd_new_panel_ili9881c(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    return ESP_ERR_NOT_SUPPORTED;
}
