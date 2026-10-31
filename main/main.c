/*
 * door_access —— 双核智能门禁（ESP32-S3 侧：感知 / 交互 / 云）
 *
 * M0：工程骨架（编译 + 启动自检）
 * M1：屏 BSP —— ST7796U 320x480 竖屏 + LVGL 9，中文标题
 * M2：触摸 BSP —— FT6336 轮询模式（G_MODE=0 释放 IO10）+ 坐标标定
 * M3：摄像头预览 —— GC0308 QVGA → LVGL image（旋转 90° 显示）
 * M4：人脸检测 —— MSRMNP_S8_V1 + 四角括号框 + 稳定 3 帧确认
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
#include "lv_fonts.h"

static const char *TAG = "door_access";

/* 摄像头出 320x240 横图，旋转 90° 后在竖屏上显示为 240x320 */
#define PREVIEW_W  240
#define PREVIEW_H  320
#define PREVIEW_ROTATE_CW  0   /* 0=逆时针（本板实测正确），1=顺时针 */

/* 图像在屏幕上的位置：水平居中，顶部 40 让给标题 */
#define PREVIEW_X  ((LCD_H_RES - PREVIEW_W) / 2)
#define PREVIEW_Y  40

#define BRACKET_ARM   26   /* 括号臂长 */
#define BRACKET_THICK 4

static lv_obj_t *s_img = NULL;
static lv_image_dsc_t s_img_dsc;
static uint8_t *s_frame_buf = NULL;     /* LVGL 图像源（小端 RGB565，PSRAM） */
static lv_obj_t *s_lbl_info = NULL;

/* 每个角两根短棒，共 8 根；检测到人脸时摆到框的四角 */
static lv_obj_t *s_bracket[8] = {0};
static int s_stable_cnt = 0;

/* ---------------- 预览界面 ---------------- */
static lv_obj_t *make_bar(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x4ce08a), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    return bar;
}

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
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, PREVIEW_Y);

    /* 四角括号：0-1 TL, 2-3 TR, 4-5 BL, 6-7 BR（每角一横一竖） */
    for (int i = 0; i < 8; i++) {
        s_bracket[i] = make_bar(scr, (i % 2 == 0) ? BRACKET_ARM : BRACKET_THICK,
                                (i % 2 == 0) ? BRACKET_THICK : BRACKET_ARM);
    }

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "人脸检测");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3ddc78), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 10);

    s_lbl_info = lv_label_create(scr);
    lv_label_set_text(s_lbl_info, "请面向屏幕");
    lv_obj_set_style_text_font(s_lbl_info, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0x7d8ea0), 0);
    lv_obj_align(s_lbl_info, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* 把摄像头坐标系的框映射到预览图像坐标（逆时针 90°）：
 *   dest_x = src_y
 *   dest_y = (src_w - 1) - src_x
 * ⚠ src_w 是**检测时的原始帧宽度 320**，不是预览宽度 240；
 *   两处写错会像差了 80px 的整位移（框跑到头发上）。 */
static void show_bracket(const face_box_t *b, int src_w)
{
    int px1 = b->y1;
    int px2 = b->y2;
    int py1 = (src_w - 1) - b->x2;
    int py2 = (src_w - 1) - b->x1;

    /* 夹紧到预览区域，防止画到图像外面 */
    if (px1 < 0) px1 = 0;
    if (py1 < 0) py1 = 0;
    if (px2 > PREVIEW_W - 1) px2 = PREVIEW_W - 1;
    if (py2 > PREVIEW_H - 1) py2 = PREVIEW_H - 1;
    if (px2 - px1 < 8 || py2 - py1 < 8) return;

    const int x1 = PREVIEW_X + px1, y1 = PREVIEW_Y + py1;
    const int x2 = PREVIEW_X + px2, y2 = PREVIEW_Y + py2;

    struct { lv_obj_t *o; int x, y; } pos[8] = {
        {s_bracket[0], x1, y1},                 /* TL 横 */
        {s_bracket[1], x1, y1},                 /* TL 竖 */
        {s_bracket[2], x2 - BRACKET_ARM, y1},   /* TR 横 */
        {s_bracket[3], x2 - BRACKET_THICK, y1}, /* TR 竖 */
        {s_bracket[4], x1, y2 - BRACKET_THICK}, /* BL 横 */
        {s_bracket[5], x1, y2 - BRACKET_ARM},   /* BL 竖 */
        {s_bracket[6], x2 - BRACKET_ARM, y2 - BRACKET_THICK}, /* BR 横 */
        {s_bracket[7], x2 - BRACKET_THICK, y2 - BRACKET_ARM}, /* BR 竖 */
    };
    for (int i = 0; i < 8; i++) {
        lv_obj_set_pos(pos[i].o, pos[i].x, pos[i].y);
        lv_obj_remove_flag(pos[i].o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_bracket(void)
{
    for (int i = 0; i < 8; i++) {
        lv_obj_add_flag(s_bracket[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------- 摄像头 + 检测任务 ---------------- */
static void camera_task(void *arg)
{
    char info[64];
    face_box_t boxes[FACE_MAX_BOXES];
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

        /* 检测用原始帧（旋转前），坐标也是摄像头坐标系 */
        const int fb_w = (int)fb->width;
        int n = face_engine_detect(fb->buf, fb_w, (int)fb->height, boxes, FACE_MAX_BOXES);
        esp_camera_fb_return(fb);

        /* 稳定帧确认：连续 N 帧都有人脸才算“确认到人”，计数封顶 N */
        if (n > 0) {
            if (s_stable_cnt < FACE_STABLE_FRAMES) s_stable_cnt++;
        } else {
            s_stable_cnt = 0;
        }

        bsp_lvgl_lock();
        if (n > 0) {
            show_bracket(&boxes[0], fb_w);
            if (s_stable_cnt >= FACE_STABLE_FRAMES) {
                snprintf(info, sizeof(info), "人脸已确认  %d/%d", s_stable_cnt, FACE_STABLE_FRAMES);
            } else {
                snprintf(info, sizeof(info), "检测稳定中  %d/%d", s_stable_cnt, FACE_STABLE_FRAMES);
            }
        } else {
            hide_bracket();
            snprintf(info, sizeof(info), "请面向屏幕");
        }
        lv_label_set_text(s_lbl_info, info);
        lv_obj_invalidate(s_img);
        bsp_lvgl_unlock();

        cnt++;
        if ((cnt % 30) == 0) {
            int64_t dt = esp_timer_get_time() - t0;
            int fps_x10 = (int)(30LL * 10000000 / dt);
            t0 = esp_timer_get_time();
            ESP_LOGI(TAG, "fps=%d.%d faces=%d stable=%d", fps_x10 / 10, fps_x10 % 10,
                     n, s_stable_cnt);
        }
    }
}

static void heartbeat_task(void *arg)
{
    uint32_t n = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
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

    ESP_ERROR_CHECK(face_engine_init());
    ESP_ERROR_CHECK(bsp_camera_init());
    bsp_lcd_set_backlight(80);

    xTaskCreatePinnedToCore(camera_task, "cam", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(heartbeat_task, "hb", 3072, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "M4 face detect started");
}
