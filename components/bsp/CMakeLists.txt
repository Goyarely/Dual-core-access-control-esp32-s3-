idf_component_register(
    SRCS "bsp_i2c.c" "bsp_pca9557.c" "bsp_lcd.c" "bsp_lvgl.c" "bsp_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gpio
             esp_lcd esp_lcd_st7796 esp_lcd_touch esp_lcd_touch_ft6x36
             lvgl esp_lvgl_port app_config
)
