#include "bsp_pca9557.h"
#include "bsp_i2c.h"
#include "esp_check.h"
#include "app_config.h"

#define REG_OUTPUT  0x01
#define REG_CONFIG  0x03

static const char *TAG = "bsp_pca";
static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_out = 0xFF;   /* 影子寄存器：位运算后整字节写回 */

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t write_out(void)
{
    return write_reg(REG_OUTPUT, s_out);
}

esp_err_t bsp_pca9557_init(void)
{
    ESP_RETURN_ON_FALSE(bsp_i2c_bus() != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c not init");

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev), TAG, "add dev");

    /* 先全配成输出，再统一置位：CS=1(不选中) PA_EN=1(功放开) PWDN=0(摄像头工作) */
    ESP_RETURN_ON_ERROR(write_reg(REG_CONFIG, 0x00), TAG, "cfg");

    /* 高 5 位没用到的脚保持 0（与 07 章实测的 0x03 一致），只操作低 3 位 */
    s_out = 0x00;
    s_out |= (1 << PCA9557_IO_LCD_CS);
    s_out |= (1 << PCA9557_IO_PA_EN);
    return write_out();
}

esp_err_t bsp_pca9557_lcd_cs(bool high)
{
    if (high) s_out |= (1 << PCA9557_IO_LCD_CS);
    else      s_out &= ~(1 << PCA9557_IO_LCD_CS);
    return write_out();
}

esp_err_t bsp_pca9557_cam_pwdn(bool power_down)
{
    if (power_down) s_out |= (1 << PCA9557_IO_DVP_PWDN);
    else            s_out &= ~(1 << PCA9557_IO_DVP_PWDN);
    return write_out();
}

esp_err_t bsp_pca9557_pa_en(bool on)
{
    if (on) s_out |= (1 << PCA9557_IO_PA_EN);
    else    s_out &= ~(1 << PCA9557_IO_PA_EN);
    return write_out();
}
