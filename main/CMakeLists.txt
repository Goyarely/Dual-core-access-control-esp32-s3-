idf_component_register(
    SRCS "main.c"
         "ui/ui_main.c"
         "link/link_task.c"
         "fonts/lv_font_misans_16.c"
         "fonts/lv_font_misans_24.c"
    INCLUDE_DIRS "." "fonts" "ui" "link"
    REQUIRES bsp lvgl app_config face_engine link esp_driver_uart
             esp_psram esp_hw_support esp_timer
)

# 生成的字库用 #include "lvgl.h" 这种简写包含
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

# 编译前自动核对字库覆盖：界面里用到但字库没有的字会显示成方块，
# 这里直接让编译失败，避免"跑起来才发现方块"。
add_custom_target(font_check ALL
    COMMAND ${CMAKE_COMMAND} -E echo "== 核对中文字库覆盖 =="
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/check_font.py
    COMMENT "check_font.py"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} font_check)
