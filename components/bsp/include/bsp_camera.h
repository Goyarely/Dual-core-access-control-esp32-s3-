#pragma once
#include "esp_err.h"
#include "esp_camera.h"

/*
 * GC0308（DVP 并口，RGB565 大端，QVGA 320x240）
 *
 * 三条硬性约束（都在 esp32s3_learn/08-dvp-camera 里踩过）：
 *   1) XCLK 必须 24MHz —— 用 OV2640 常用的 20MHz 会出花屏，
 *      现象极具误导性：屏和 SPI 都正常，纯色能铺满，但画面上全是噪点
 *   2) PWDN 在 PCA9557 IO2，低有效；不拉低则 SCCB 完全不应答
 *   3) SCCB 复用已有 I2C0（pin_sccb_sda = -1 + sccb_i2c_port = 0），
 *      否则 esp32-camera 会自建总线把 PCA9557 顶掉
 */
esp_err_t bsp_camera_init(void);

/**
 * @brief 一帧 RGB565（大端）→ LVGL 图像缓冲（小端），同时顺时针/逆时针旋转 90°
 *
 * 传感器在板上是横装的、屏幕是竖屏，所以预览必须转 90°。
 * 旋转和字节交换在同一趟扫描里完成（dst 顺序写、src 跨行走，PSRAM 上可接受）。
 *
 * @param cw true = 顺时针 90°，false = 逆时针 90°
 */
void bsp_camera_blit(uint8_t *dst, const uint8_t *src, int src_w, int src_h, bool cw);
