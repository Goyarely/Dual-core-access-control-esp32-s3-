#pragma once
#include "esp_err.h"
#include "esp_lcd_touch.h"

/*
 * FT6336U 电容触摸（I2C 0x38）
 *
 * ⚠ 本板 TP-INT 接 IO10，而 IO10 又被板载 LED6 占用、且本工程要拿它当 UART1_TX。
 *   所以初始化时写 G_MODE(0xA4)=0 把触摸控制器切到「轮询模式」，
 *   它就不会再驱动 IO10，IO10 从此归 UART 所有。
 */
esp_err_t bsp_touch_init(void);
esp_lcd_touch_handle_t bsp_touch_handle(void);
