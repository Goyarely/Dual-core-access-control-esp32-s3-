#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"

/*
 * ST7796U 3.5 寸 IPS，320x480 竖屏原生、必须 BGR、必须 INVON。
 * CS 走 PCA9557 IO0，RST 接板级复位网络（reset_gpio_num = -1）。
 */
esp_err_t bsp_lcd_init(void);

esp_lcd_panel_handle_t bsp_lcd_panel(void);
esp_lcd_panel_io_handle_t bsp_lcd_io(void);

/** 背光 0~100% */
esp_err_t bsp_lcd_set_backlight(int percent);
