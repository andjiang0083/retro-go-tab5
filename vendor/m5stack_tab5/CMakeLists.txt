idf_component_register(
    SRCS "m5stack_tab5.c" "esp_lcd_st7123.c" "esp_lcd_ili9881c.c"
    INCLUDE_DIRS "include"
    INCLUDE_DIRS "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    
    REQUIRES
        driver
        esp_lcd
        esp_codec_dev
        esp_lcd_st7121
        esp_lcd_touch_st7123
    
    PRIV_REQUIRES
        spiffs
        fatfs
)

# No LVGL mode
target_compile_definitions(${COMPONENT_LIB} PRIVATE BSP_CONFIG_NO_GRAPHIC_LIB=1)
