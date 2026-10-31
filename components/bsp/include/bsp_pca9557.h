#pragma once
#include "esp_err.h"
#include <stdbool.h>

/*
 * PCA9557（0x19）—— 本板三根关键控制脚都在这里
 *   IO0 = LCD_CS   上电默认输入（高阻）→ CS 被 R53 拉高 → 屏收不到命令
 *   IO1 = PA_EN    功放使能，高有效
 *   IO2 = DVP_PWDN 摄像头掉电，低有效（不拉低则 SCCB 完全不应答）
 *
 * ⚠ 独立供电，寄存器跨 ESP32 复位不丢；设错了要拔电才恢复。
 */
esp_err_t bsp_pca9557_init(void);
esp_err_t bsp_pca9557_lcd_cs(bool high);
esp_err_t bsp_pca9557_cam_pwdn(bool power_down);
esp_err_t bsp_pca9557_pa_en(bool on);
