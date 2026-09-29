idf_component_register(
    SRCS "ppa_engine.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_driver_ppa esp_mm
)
