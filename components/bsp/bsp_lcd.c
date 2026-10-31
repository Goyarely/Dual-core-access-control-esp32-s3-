#include "bsp_lcd.h"
#include "bsp_pca9557.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_st7796.h"
#include "app_config.h"

static const char *TAG = "bsp_lcd";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

#define BL_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER  LEDC_TIMER_1
#define BL_LEDC_CH     LEDC_CHANNEL_1
#define BL_DUTY_MAX    1023

esp_err_t bsp_lcd_init(void)
{
    ESP_RETURN_ON_FALSE(s_panel == NULL, ESP_OK, TAG, "already init");

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,       /* 屏只写不读 */
        .sclk_io_num = LCD_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_NC,       /* CS 在 PCA9557 上，手动控制 */
        .dc_gpio_num = LCD_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,           /* 10MHz 实测稳定；20MHz 会撕裂 */
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &s_io), TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(s_io, &panel_cfg, &s_panel), TAG, "st7796");

    /* 初始化序列，顺序不能改：MADCTL 在 panel_init 时才下发。
     * 注意：走 LVGL 时这里的方向会被 esp_lvgl_port 按 disp_cfg.rotation 重新覆盖，
     * 真正的方向配置在 bsp_lvgl.c；这里保持一致只是为了不接 LVGL 时也正确。 */
    ESP_RETURN_ON_ERROR(bsp_pca9557_lcd_cs(false), TAG, "cs low");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    /* 背光 PWM：高有效，5000Hz 无频闪 */
    ledc_timer_config_t t = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");

    ledc_channel_config_t c = {
        .gpio_num = LCD_BL_GPIO,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CH,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&c), TAG, "ledc ch");

    ESP_LOGI(TAG, "ST7796U %dx%d portrait ready (BGR + INVON, no mirror)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_lcd_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t bsp_lcd_io(void)
{
    return s_io;
}

esp_err_t bsp_lcd_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)BL_DUTY_MAX * percent / 100;
    return ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CH, duty) == ESP_OK
               ? ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CH)
               : ESP_FAIL;
}
