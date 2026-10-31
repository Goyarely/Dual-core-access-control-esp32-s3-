/*
 * door_access —— 双核智能门禁（ESP32-S3 侧：感知 / 交互 / 云）
 *
 * M0：工程骨架（编译 + 启动自检）
 * M1：屏 BSP —— ST7796U 320x480 竖屏 + LVGL 9，中文标题
 * M2：触摸 BSP —— FT6336 轮询模式（G_MODE=0 释放 IO10）+ 坐标标定
 * M3：摄像头预览 —— GC0308 QVGA → LVGL image 实时刷新
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
#include "lv_fonts.h"

static const char *TAG = "door_access";

/* 摄像头出 320x240 横图，旋转 90° 后在竖屏上显示为 240x320 */
#define PREVIEW_W  240
#define PREVIEW_H  320
#define PREVIEW_ROTATE_CW  0   /* 0=逆时针（本板实测正确），1=顺时针 */

static lv_obj_t *s_img = NULL;
static lv_image_dsc_t s_img_dsc;
static uint8_t *s_frame_buf = NULL;     /* LVGL 图像源（小端 RGB565，PSRAM） */
static lv_obj_t *s_lbl_fps = NULL;

/* ---------------- 预览界面 ---------------- */
static void ui_camera_preview(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d131b), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w = PREVIEW_W;
    s_img_dsc.header.h = PREVIEW_H;
    s_img_dsc.header.stride = PREVIEW_W * 2;
    s_img_dsc.data_size = PREVIEW_W * PREVIEW_H * 2;
    s_img_dsc.data = s_frame_buf;

    s_img = lv_image_create(scr);
    lv_image_set_src(s_img, &s_img_dsc);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "摄像头预览");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3ddc78), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 10);

    s_lbl_fps = lv_label_create(scr);
    lv_label_set_text(s_lbl_fps, "waiting...");
    lv_obj_set_style_text_font(s_lbl_fps, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(s_lbl_fps, lv_color_hex(0x7d8ea0), 0);
    lv_obj_align(s_lbl_fps, LV_ALIGN_BOTTOM_LEFT, 8, -8);
}

/* ---------------- 摄像头任务 ---------------- */
static void camera_task(void *arg)
{
    char buf[48];
    int cnt = 0;
    int64_t t0 = esp_timer_get_time();

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* 一帧搞定三件事：BE→LE 换字节 + 旋转 90° + 写入 LVGL 图像缓冲 */
        bsp_camera_blit(s_frame_buf, fb->buf, fb->width, fb->height, PREVIEW_ROTATE_CW);
        esp_camera_fb_return(fb);

        bsp_lvgl_lock();
        lv_obj_invalidate(s_img);
        bsp_lvgl_unlock();

        cnt++;
        if ((cnt % 30) == 0) {
            int64_t dt = esp_timer_get_time() - t0;
            int fps_x10 = (int)(30LL * 10000000 / dt);
            t0 = esp_timer_get_time();
            snprintf(buf, sizeof(buf), "fps=%d.%d  frames=%d", fps_x10 / 10, fps_x10 % 10, cnt);

            bsp_lvgl_lock();
            lv_label_set_text(s_lbl_fps, buf);
            bsp_lvgl_unlock();

            ESP_LOGI(TAG, "%s", buf);
        }
    }
}

static void heartbeat_task(void *arg)
{
    uint32_t n = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
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

    /* LVGL 图像源缓冲：PSRAM（LVGL 软件渲染时读它，不经 DMA） */
    s_frame_buf = (uint8_t *)heap_caps_malloc(PREVIEW_W * PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_frame_buf ? ESP_OK : ESP_ERR_NO_MEM);

    bsp_lvgl_lock();
    ui_camera_preview();
    bsp_lvgl_unlock();

    ESP_ERROR_CHECK(bsp_camera_init());
    bsp_lcd_set_backlight(80);

    xTaskCreatePinnedToCore(camera_task, "cam", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(heartbeat_task, "hb", 3072, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "M3 camera preview started");
}
