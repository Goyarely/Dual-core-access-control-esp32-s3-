#include "bsp_touch.h"
#include "bsp_i2c.h"
#include "bsp_lvgl.h"
#include "esp_lcd_touch_ft6x36.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "app_config.h"
#include <stdlib.h>

#define FT6X36_REG_G_MODE   0xA4   /* 0=轮询(不驱动 INT)  1=触发(中断) */
#define FT6X36_REG_POINTRATE 0x88  /* 扫描周期，驱动默认写 0x0E(约14ms)，这里提到 0x05 */

static const char *TAG = "bsp_touch";

static esp_lcd_touch_handle_t s_tp = NULL;
static esp_lcd_panel_io_handle_t s_tp_io = NULL;

/* 坐标映射（2026-10-31 四角实测，竖屏 320x480）
 *
 *   按的位置(屏幕)      控制器报的原始值
 *   TL   (38, 38)  ←  (49, 43)
 *   TR  (282, 38)  ←  (274, 39)
 *   BL   (38,442)  ←  (57, 449)
 *   BR  (282,442)  ←  (280, 454)
 *
 * 结论：本板 FT6336 报出来的**已经是屏幕像素坐标**（斜率和偏移都接近 1:1，
 * 残差 ±10px 是指尖落点误差），既不用换轴也不用翻转——这和 face_lvgl 横屏时
 * 必须 swap+mirror 的情况完全不同。所以这里只做边界夹紧，不做变换。 */
static void touch_map(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                      uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    for (int i = 0; i < *point_num; i++) {
        if (x[i] > LCD_H_RES - 1) x[i] = LCD_H_RES - 1;
        if (y[i] > LCD_V_RES - 1) y[i] = LCD_V_RES - 1;
    }

    /* 落点日志：移动超过 15px 才打一次 */
    static int lx = -1000, ly = -1000;
    if (*point_num && (abs((int)x[0] - lx) > 15 || abs((int)y[0] - ly) > 15)) {
        lx = x[0];
        ly = y[0];
        ESP_LOGI(TAG, "touch x=%d y=%d", lx, ly);
    }
}

esp_err_t bsp_touch_init(void)
{
    ESP_RETURN_ON_FALSE(s_tp == NULL, ESP_OK, TAG, "already init");
    ESP_RETURN_ON_FALSE(bsp_i2c_bus() != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c not init");

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG();
    io_cfg.scl_speed_hz = I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bsp_i2c_bus(), &io_cfg, &s_tp_io), TAG, "tp io");

    /* 坐标映射放在 process_coordinates 里做，驱动内置的 mirror/swap 全关 */
    esp_lcd_touch_config_t cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,      /* 不用中断 */
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .process_coordinates = touch_map,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft6x36(s_tp_io, &cfg, &s_tp), TAG, "ft6x36 init");

    /* 关键一步：切轮询模式，把 IO10 让给 UART1_TX */
    uint8_t g_mode = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_tp_io, FT6X36_REG_G_MODE, &g_mode, 1),
                        TAG, "set g_mode");

    /* 提高触摸扫描率：默认 0x0E 约 10Hz，点一下要等 100ms 才被读到 */
    uint8_t rate = 0x05;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_tp_io, FT6X36_REG_POINTRATE, &rate, 1),
                        TAG, "set point rate");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = bsp_lvgl_display(),
        .handle = s_tp,
        .scale = {.x = 1.0f, .y = 1.0f},
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg) != NULL, ESP_FAIL, TAG, "add touch");

    ESP_LOGI(TAG, "FT6336 polling mode (G_MODE=0), IO10 released to UART");
    return ESP_OK;
}

esp_lcd_touch_handle_t bsp_touch_handle(void)
{
    return s_tp;
}
