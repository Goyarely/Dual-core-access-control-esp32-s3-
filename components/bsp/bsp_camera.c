#include "bsp_camera.h"
#include "bsp_pca9557.h"
#include "esp_check.h"
#include "esp_log.h"
#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_camera";

esp_err_t bsp_camera_init(void)
{
    /* 先给一个 PWDN 脉冲，再上电。
     * PCA9557 是独立供电、寄存器跨 ESP32 复位不丢，所以 ESP32 热复位时
     * 传感器始终带着电、XCLK 在上次复位瞬间断掉，它会卡在半帧状态；
     * 这时直接 esp_camera_init 能识别到 0x21，但一帧都收不到
     * （现象：cam_hal: Failed to get frame: timeout，2026-10-31 实测）。
     * 拉高 PWDN 10ms 再放开，等于给它做了次真上电。 */
    ESP_RETURN_ON_ERROR(bsp_pca9557_cam_pwdn(true), TAG, "pwdn high");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(bsp_pca9557_cam_pwdn(false), TAG, "pwdn low");
    vTaskDelay(pdMS_TO_TICKS(50));

    camera_config_t cfg = {
        .pin_pwdn = -1,                     /* 由 PCA9557 控制，不走 GPIO */
        .pin_reset = -1,
        .pin_xclk = CAM_XCLK_GPIO,

        .pin_sccb_sda = -1,                 /* 复用已有 I2C0 */
        .pin_sccb_scl = -1,
        .sccb_i2c_port = 0,

        .pin_d7 = CAM_D7_GPIO, .pin_d6 = CAM_D6_GPIO,
        .pin_d5 = CAM_D5_GPIO, .pin_d4 = CAM_D4_GPIO,
        .pin_d3 = CAM_D3_GPIO, .pin_d2 = CAM_D2_GPIO,
        .pin_d1 = CAM_D1_GPIO, .pin_d0 = CAM_D0_GPIO,
        .pin_vsync = CAM_VSYNC_GPIO,
        .pin_href = CAM_HREF_GPIO,
        .pin_pclk = CAM_PCLK_GPIO,

        .xclk_freq_hz = CAM_XCLK_HZ,        /* 24MHz，不能改 20MHz */
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .fb_count = CAM_FB_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t ret = esp_camera_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (检查 PCA9557 IO2 是否拉低 / XCLK 24MHz)",
                 esp_err_to_name(ret));
        return ret;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_hmirror(s, 1);            /* 自拍视角，画面像照镜子 */

    ESP_LOGI(TAG, "GC0308 QVGA RGB565 ready, xclk=%d MHz, fb=%d(PSRAM)",
             CAM_XCLK_HZ / 1000000, CAM_FB_COUNT);
    return ESP_OK;
}

void bsp_camera_blit(uint8_t *dst, const uint8_t *src, int src_w, int src_h, bool cw)
{
    /* 旋转 90° 后目标宽高互换：320x240 → 240x320 */
    const int dst_w = src_h;
    const int dst_h = src_w;

    for (int y = 0; y < dst_h; y++) {            /* 目标行，顺序写 */
        uint8_t *d = dst + (size_t)y * dst_w * 2;
        for (int x = 0; x < dst_w; x++) {        /* 目标列 */
            /* 目标(x,y) 反查源坐标：
             *   CW : src_x = y,        src_y = src_h-1-x
             *   CCW: src_x = src_w-1-y, src_y = x        */
            int sx = cw ? y : (src_w - 1 - y);
            int sy = cw ? (src_h - 1 - x) : x;
            const uint8_t *s = src + ((size_t)sy * src_w + sx) * 2;
            *d++ = s[1];                         /* BE → LE */
            *d++ = s[0];
        }
    }
}
