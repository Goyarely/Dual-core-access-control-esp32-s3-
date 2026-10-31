/*
 * door_access —— 双核智能门禁（ESP32-S3 侧：感知 / 交互 / 云）
 *
 * M0：工程骨架（编译 + 启动自检）
 * M1：屏 BSP —— ST7796U 320x480 竖屏 + LVGL 9
 * M2：触摸 BSP —— FT6336 轮询（G_MODE=0 释放 IO10）
 * M3：摄像头预览 —— GC0308 QVGA，旋转 90° 显示
 * M4：人脸检测 —— MSRMNP_S8_V1 + 四角括号 + 稳定 3 帧
 * M5：人脸识别 —— MFN_S8_V1 + /storage 人脸库
 * M6：UI 八界面状态机（main/ui/）
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "app_config.h"
#include "bsp_i2c.h"
#include "bsp_pca9557.h"
#include "bsp_lcd.h"
#include "bsp_lvgl.h"
#include "bsp_touch.h"
#include "bsp_camera.h"
#include "face_engine.h"
#include "ui_main.h"
#include "app_settings.h"

static const char *TAG = "door_access";

/* 摄像头出 320x240 横图，旋转 90° 后显示为 240x320 */
#define PREVIEW_W  240
#define PREVIEW_H  320
#define PREVIEW_ROTATE_CW  0   /* 0=逆时针（本板实测正确） */

static uint8_t *s_frame_buf = NULL;

/* ---------------- 摄像头 + 识别任务 ---------------- */
static void camera_task(void *arg)
{
    face_box_t boxes[FACE_MAX_BOXES];
    int cnt = 0;
    int64_t t0 = esp_timer_get_time();

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* BE→LE + 旋转 90° + 写入 LVGL 图像缓冲 */
        bsp_camera_blit(s_frame_buf, fb->buf, fb->width, fb->height, PREVIEW_ROTATE_CW);

        /* 检测在旋转后的竖屏图像上做（模型看到正立人脸，相似度才上得去） */
        int n = face_engine_detect(fb->buf, fb->width, fb->height, boxes, FACE_MAX_BOXES);

        /* 先让 UI 吃掉这一帧，再由它决定要不要识别/录入 */
        bsp_lvgl_lock();
        ui_frame_feed(n, (n > 0) ? &boxes[0] : NULL);
        bool need_recog = ui_take_recognize_request();
        bool need_enroll = ui_take_enroll_request();
        bsp_lvgl_unlock();

        if (need_recog) {
            char name[24] = {0};
            float sim = 0;
            uint16_t id = 0;
            esp_err_t r = face_recog_recognize(fb->buf, fb->width, fb->height,
                                               name, sizeof(name), &sim, &id);
            ui_recog_t res = UI_RECOG_STRANGER;
            /* 设置页的相似度阈值在这里真正生效（只能比库内阈值更严） */
            if (r == ESP_OK && sim >= app_settings()->sim_thr) res = UI_RECOG_HIT;
            else if (r == ESP_ERR_INVALID_STATE) res = UI_RECOG_NOFACE;

            char title[32] = {0}, sub[48] = {0};
            if (res == UI_RECOG_HIT) {
                /* 库内 id 只增不减，界面上换算成"第几个成员" */
                uint16_t ids[MEMBER_MAX];
                int cnt = face_members(ids, MEMBER_MAX);
                int pos = 1;
                for (int i = 0; i < cnt; i++) {
                    if (ids[i] == id) { pos = i + 1; break; }
                }
                snprintf(title, sizeof(title), "成员 %d", pos);
                snprintf(sub, sizeof(sub), "编号 %02d · 相似度 %.2f", pos, sim);
            }
            bsp_lvgl_lock();
            ui_recog_result(res, title, sub, sim);
            bsp_lvgl_unlock();
        }

        if (need_enroll) {
            /* 先识别一遍：库里已有人就不重复登记（否则同一个人会占多个 id） */
            char nm[24] = {0};
            float sim = 0;
            uint16_t dup_id = 0;
            esp_err_t rr = face_recog_recognize(fb->buf, fb->width, fb->height,
                                                nm, sizeof(nm), &sim, &dup_id);
            bool dup = (rr == ESP_OK && sim >= app_settings()->sim_thr);

            bsp_lvgl_lock();
            if (dup) {
                char msg[48];
                snprintf(msg, sizeof(msg), "已录入过 %.2f", sim);
                ui_enroll_msg(false, msg);
            } else {
                esp_err_t r = face_recog_enroll(fb->buf, fb->width, fb->height);
                ui_enroll_result(r == ESP_OK, face_recog_count());
            }
            bsp_lvgl_unlock();
            ESP_LOGW(TAG, "enroll: dup=%d", (int)dup);
        }

        esp_camera_fb_return(fb);

        cnt++;
        if ((cnt % 60) == 0) {
            int64_t dt = esp_timer_get_time() - t0;
            int fps_x10 = (int)(60LL * 10000000 / dt);
            t0 = esp_timer_get_time();
            ESP_LOGI(TAG, "fps=%d.%d faces=%d state=%d lib=%d",
                     fps_x10 / 10, fps_x10 % 10, n, (int)ui_state(), face_recog_count());
        }
    }
}

/* 1 秒节拍：锁定倒计时、成功/拒绝页自动回落 */
static void ui_tick_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bsp_lvgl_lock();
        ui_tick_1s();
        bsp_lvgl_unlock();
    }
}

static void heartbeat_task(void *arg)
{
    uint32_t n = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGI(TAG, "alive #%lu  dram=%uKB  psram=%uKB",
                 (unsigned long)++n,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "================ door_access %s ================", APP_VERSION);
    ESP_LOGI(TAG, "target=%s  cores=%d  rev=v%d.%d", CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "flash=16MB  psram=%uKB", (unsigned)(esp_psram_get_size() / 1024));

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_pca9557_init());
    ESP_LOGI(TAG, "i2c + pca9557 ok");

    ESP_ERROR_CHECK(bsp_lcd_init());
    ESP_ERROR_CHECK(bsp_lvgl_init());
    ESP_ERROR_CHECK(bsp_touch_init());

    s_frame_buf = (uint8_t *)heap_caps_malloc(PREVIEW_W * PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_frame_buf ? ESP_OK : ESP_ERR_NO_MEM);

    bsp_lvgl_lock();
    ui_init(s_frame_buf, PREVIEW_W, PREVIEW_H);
    ui_set_state(UI_STANDBY);
    bsp_lvgl_unlock();

    ESP_ERROR_CHECK(face_engine_init());
    ESP_ERROR_CHECK(face_recog_init());
    ESP_ERROR_CHECK(bsp_camera_init());
    bsp_lcd_set_backlight(80);

    xTaskCreatePinnedToCore(camera_task, "cam", 8192, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(ui_tick_task, "ui_tick", 3072, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(heartbeat_task, "hb", 3072, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "M6 UI ready, %d faces in lib", face_recog_count());
}
