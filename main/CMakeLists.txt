idf_component_register(
    SRCS "main.c"
         "fonts/lv_font_misans_16.c"
         "fonts/lv_font_misans_24.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES bsp lvgl app_config esp_psram esp_hw_support
)

# 生成的字库用 #include "lvgl.h" 这种简写包含
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
