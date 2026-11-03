#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_MAX_BOXES  4

typedef struct {
    /* 坐标已转成竖屏空间（240x320），与 LVGL 预览图像坐标一一对应，
     * UI 侧直接用、不需要再做 90° 反变换 */
    int   x1, y1, x2, y2;
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

/* ---------------- 识别 / 人脸库 ----------------
 * 人脸库是 FAT 分区（storage）上的 /storage/face.db，掉电不丢。
 * 录一个人 → Person_0，再录一个 → Person_1 … */

/** 挂载 /storage 并加载 MFN_S8_V1 识别模型（首次调用可能耗时） */
esp_err_t face_recog_init(void);

/** 把当前帧里检测到的人脸注册进库；ESP_ERR_NOT_FOUND = 画面里没人脸 */
esp_err_t face_recog_enroll(const uint8_t *buf, int w, int h);

/** 识别当前帧；ESP_OK = 命中（name_out/score_out 有效），ESP_ERR_NOT_FOUND = 陌生人/无人脸 */
esp_err_t face_recog_recognize(const uint8_t *buf, int w, int h,
                               char *name_out, size_t name_len, float *score_out,
                               uint16_t *id_out);

/** 清空人脸库 */
esp_err_t face_recog_clear(void);

/** 库里的人数（有效特征数） */
int face_recog_count(void);

/**
 * @brief 列出已登记成员的 id（升序）
 *
 * 识别库的 id 规则是 num_feats_total+1、只增不减，库本身不提供枚举接口，
 * 所以在 /storage/members.bin 里自己维护一份 id 索引。
 * @return 实际写入 ids 的个数
 */
int face_members(uint16_t *ids, int max);

/** 删除指定 id 的成员（同时更新索引文件） */
esp_err_t face_delete_member(uint16_t id);


#ifdef __cplusplus
}
#endif
