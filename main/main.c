/*
 * door_access —— 双核智能门禁（ESP32-S3 侧：感知 / 交互 / 云）
 *
 * M0：工程骨架（编译 + 启动自检）
 * M1：屏 BSP —— ST7796U 320x480 竖屏 + LVGL 9，中文标题
 * M2：触摸 BSP —— FT6336 轮询模式（G_MODE=0 释放 IO10）+ 坐标标定
 * M3：摄像头预览 —— GC0308 QVGA → LVGL image（旋转 90° 显示）
 * M4：人脸检测 —— MSRMNP_S8_V1 + 四角括号框 + 稳定 3 帧确认
 * M5：人脸识别 —— MFN_S8_V1 + /storage 人脸库（录入/识别/重置）
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

#define PREVIEW_X  ((LCD_H_RES - PREVIEW_W) / 2)
#define PREVIEW_Y  40

#define BRACKET_ARM   26
#define BRACKET_THICK 4

static lv_obj_t *s_img = NULL;
static lv_image_dsc_t s_img_dsc;
static uint8_t *s_frame_buf = NULL;
static lv_obj_t *s_lbl_info = NULL;
static lv_obj_t *s_lbl_result = NULL;

static lv_obj_t *s_bracket[8] = {0};
static int s_stable_cnt = 0;

/* 按钮命令：触摸只写这里，实际执行在 camera_task 里（拿着最新一帧） */
typedef enum { CMD_NONE = 0, CMD_ENROLL, CMD_RECOGNIZE, CMD_CLEAR } cmd_t;
static volatile cmd_t s_cmd = CMD_NONE;

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

static void btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_cmd = (cmd_t)(intptr_t)lv_event_get_user_data(e);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int x, cmd_t cmd)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 92, 40);
    lv_obj_set_pos(btn, x, PREVIEW_Y + PREVIEW_H + 46);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x185fa5), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_misans_16, 0);
    lv_obj_center(lbl);
    return btn;
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

    for (int i = 0; i < 8; i++) {
        s_bracket[i] = make_bar(scr, (i % 2 == 0) ? BRACKET_ARM : BRACKET_THICK,
                                (i % 2 == 0) ? BRACKET_THICK : BRACKET_ARM);
    }

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "人脸门禁");
    lv_obj_set_style_text_font(title, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3ddc78), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 10);

    /* 识别结果放右上角：命中显示 Person_x + 相似度 */
    s_lbl_result = lv_label_create(scr);
    lv_label_set_text(s_lbl_result, "");
    lv_obj_set_style_text_font(s_lbl_result, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(s_lbl_result, lv_color_hex(0xffd166), 0);
    lv_obj_align(s_lbl_result, LV_ALIGN_TOP_RIGHT, -8, 10);

    s_lbl_info = lv_label_create(scr);
    lv_label_set_text(s_lbl_info, "请面向屏幕");
    lv_obj_set_style_text_font(s_lbl_info, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0x7d8ea0), 0);
    lv_obj_set_pos(s_lbl_info, 8, PREVIEW_Y + PREVIEW_H + 12);

    make_btn(scr, "录入", 12, CMD_ENROLL);
    make_btn(scr, "识别", 114, CMD_RECOGNIZE);
    make_btn(scr, "重置", 216, CMD_CLEAR);
}

/* face_engine 输出的框已经在竖屏预览空间里（画面转正后才进模型），
 * 这里只做边界夹紧，不再有 90° 反变换 */
static void show_bracket(const face_box_t *b)
{
    int px1 = b->x1;
    int px2 = b->x2;
    int py1 = b->y1;
    int py2 = b->y2;

    if (px1 < 0) px1 = 0;
    if (py1 < 0) py1 = 0;
    if (px2 > PREVIEW_W - 1) px2 = PREVIEW_W - 1;
    if (py2 > PREVIEW_H - 1) py2 = PREVIEW_H - 1;
    if (px2 - px1 < 8 || py2 - py1 < 8) return;

    const int x1 = PREVIEW_X + px1, y1 = PREVIEW_Y + py1;
    const int x2 = PREVIEW_X + px2, y2 = PREVIEW_Y + py2;

    struct { lv_obj_t *o; int x, y; } pos[8] = {
        {s_bracket[0], x1, y1},
        {s_bracket[1], x1, y1},
        {s_bracket[2], x2 - BRACKET_ARM, y1},
        {s_bracket[3], x2 - BRACKET_THICK, y1},
        {s_bracket[4], x1, y2 - BRACKET_THICK},
        {s_bracket[5], x1, y2 - BRACKET_ARM},
        {s_bracket[6], x2 - BRACKET_ARM, y2 - BRACKET_THICK},
        {s_bracket[7], x2 - BRACKET_THICK, y2 - BRACKET_ARM},
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

/* ---------------- 命令执行（在 camera_task 里，拿着最新一帧） ---------------- */
static void exec_cmd(cmd_t cmd, camera_fb_t *fb)
{
    char msg[48] = {0};

    switch (cmd) {
    case CMD_ENROLL: {
        esp_err_t r = face_recog_enroll(fb->buf, fb->width, fb->height);
        if (r == ESP_OK) {
            snprintf(msg, sizeof(msg), "已录入 共%d人", face_recog_count());
        } else if (r == ESP_ERR_NOT_FOUND) {
            snprintf(msg, sizeof(msg), "无人脸，录入失败");
        } else {
            snprintf(msg, sizeof(msg), "录入失败 %d", r);
        }
        break;
    }
    case CMD_RECOGNIZE: {
        char name[24] = {0};
        float sim = 0;
        esp_err_t r = face_recog_recognize(fb->buf, fb->width, fb->height, name, sizeof(name), &sim);
        if (r == ESP_OK) {
            snprintf(msg, sizeof(msg), "%s %.2f", name, sim);
        } else if (r == ESP_ERR_NOT_FOUND) {
            snprintf(msg, sizeof(msg), "陌生人");
        } else {
            snprintf(msg, sizeof(msg), "识别失败 %d", r);
        }
        break;
    }
    case CMD_CLEAR:
        face_recog_clear();
        snprintf(msg, sizeof(msg), "库已清空");
        break;
    default:
        return;
    }

    ESP_LOGI(TAG, "cmd -> %s", msg);
    bsp_lvgl_lock();
    lv_label_set_text(s_lbl_result, msg);
    bsp_lvgl_unlock();
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

        /* 命令优先：用这一帧执行，避免动作和画面错位 */
        cmd_t cmd = s_cmd;
        if (cmd != CMD_NONE) {
            s_cmd = CMD_NONE;
            exec_cmd(cmd, fb);
        }

        bsp_camera_blit(s_frame_buf, fb->buf, fb->width, fb->height, PREVIEW_ROTATE_CW);

        const int fb_w = (int)fb->width;
        int n = face_engine_detect(fb->buf, fb_w, (int)fb->height, boxes, FACE_MAX_BOXES);
        esp_camera_fb_return(fb);

        if (n > 0) {
            if (s_stable_cnt < FACE_STABLE_FRAMES) s_stable_cnt++;
        } else {
            s_stable_cnt = 0;
        }

        bsp_lvgl_lock();
        if (n > 0) {
            show_bracket(&boxes[0]);
            if (s_stable_cnt >= FACE_STABLE_FRAMES) {
                snprintf(info, sizeof(info), "人脸已确认 %d/%d", s_stable_cnt, FACE_STABLE_FRAMES);
                lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0x4ce08a), 0);
            } else {
                snprintf(info, sizeof(info), "检测稳定中 %d/%d", s_stable_cnt, FACE_STABLE_FRAMES);
                lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0xffd166), 0);
            }
        } else {
            hide_bracket();
            snprintf(info, sizeof(info), "请面向屏幕");
            lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0x7d8ea0), 0);
        }
        lv_label_set_text(s_lbl_info, info);
        lv_obj_invalidate(s_img);
        bsp_lvgl_unlock();

        cnt++;
        if ((cnt % 30) == 0) {
            int64_t dt = esp_timer_get_time() - t0;
            int fps_x10 = (int)(30LL * 10000000 / dt);
            t0 = esp_timer_get_time();
            ESP_LOGI(TAG, "fps=%d.%d faces=%d stable=%d lib=%d",
                     fps_x10 / 10, fps_x10 % 10, n, s_stable_cnt, face_recog_count());
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

    s_frame_buf = (uint8_t *)heap_caps_malloc(PREVIEW_W * PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_frame_buf ? ESP_OK : ESP_ERR_NO_MEM);

    bsp_lvgl_lock();
    ui_camera_preview();
    bsp_lvgl_unlock();

    ESP_ERROR_CHECK(face_engine_init());
    ESP_ERROR_CHECK(face_recog_init());
    ESP_ERROR_CHECK(bsp_camera_init());
    bsp_lcd_set_backlight(80);

    xTaskCreatePinnedToCore(camera_task, "cam", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(heartbeat_task, "hb", 3072, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "M5 recognize ready, %d faces in lib", face_recog_count());
}
