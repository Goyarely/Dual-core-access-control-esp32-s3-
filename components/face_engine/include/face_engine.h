#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_MAX_BOXES  4

typedef struct {
    int   x1, y1, x2, y2;   /* 摄像头原始帧坐标（320x240） */
    float score;
} face_box_t;

/** 加载 MSRMNP_S8_V1 检测模型（重复调用无副作用） */
esp_err_t face_engine_init(void);

/**
 * @brief 对一帧 RGB565（大端）做检测
 * @param buf        摄像头帧数据（PIXFORMAT_RGB565，大端）
 * @param w,h        帧尺寸
 * @param out        输出框数组
 * @param max_boxes  数组容量
 * @return 检测到的人脸数（0~max_boxes）
 */
int face_engine_detect(const uint8_t *buf, int w, int h, face_box_t *out, int max_boxes);

#ifdef __cplusplus
}
#endif
