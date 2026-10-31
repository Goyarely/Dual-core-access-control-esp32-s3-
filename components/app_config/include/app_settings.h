#pragma once
#include "esp_err.h"
#include <stdbool.h>

/*
 * 运行期可调参数（管理员菜单里能改，存 /storage/settings.bin，掉电保持）
 *
 * 注意：这里是"可调副本"，编译期默认值在 app_config.h。
 * 人脸库所在 FAT 分区由 face_engine 挂载，所以必须在 face_recog_init() 之后调用。
 */
typedef struct {
    float sim_thr;      /* 相似度阈值 0.40~0.90 */
    int   fail_max;     /* 连续失败上限 1~10 */
    int   lockout_sec;  /* 锁定秒数 10~300 */
} app_settings_t;

/** 取当前配置（首次调用会尝试从 /storage 读入，读不到用默认值） */
app_settings_t *app_settings(void);

/** 写回 /storage/settings.bin */
esp_err_t app_settings_save(void);

/** 恢复默认值（不自动保存） */
void app_settings_reset_default(void);
