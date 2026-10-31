#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"

/* I2C0：PCA9557 / 触摸 FT6336 / 摄像头 SCCB 共用一条总线 */
esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_bus(void);
