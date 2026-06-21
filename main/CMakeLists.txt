idf_component_register(
    SRCS "main.c" "board.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_psram
)
