#pragma once
#include "esp_err.h"
#include "lvgl.h"

/* LVGL 9 移植层：任务 + 递归锁 + 显示注册（320x480 竖屏，RGB565） */
esp_err_t bsp_lvgl_init(void);

lv_display_t *bsp_lvgl_display(void);

/* 供非 LVGL 任务使用；LVGL 任务内部已持锁，不要嵌套调用 */
esp_err_t bsp_lvgl_lock(void);
void bsp_lvgl_unlock(void);
