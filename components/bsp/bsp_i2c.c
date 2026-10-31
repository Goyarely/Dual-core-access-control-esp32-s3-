#include "bsp_i2c.h"
#include "esp_check.h"
#include "app_config.h"

static const char *TAG = "bsp_i2c";
static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t bsp_i2c_init(void)
{
    if (s_bus) return ESP_OK;

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "i2c bus");
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void)
{
    return s_bus;
}
