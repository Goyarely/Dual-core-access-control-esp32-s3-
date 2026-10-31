/*
 * door_access —— 双核智能门禁（ESP32-S3 侧：感知 / 交互 / 云）
 *
 * M0：工程骨架（编译 + 启动自检）
 * M1：屏 BSP —— ST7796U 320x480 竖屏 + LVGL 9，中文标题
 * M2：触摸 BSP —— FT6336 轮询模式（G_MODE=0 释放 IO10）+ 坐标标定
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "bsp_i2c.h"
#include "bsp_pca9557.h"
#include "bsp_lcd.h"
#include "bsp_lvgl.h"
#include "bsp_touch.h"
#include "lv_fonts.h"

static const char *TAG = "door_access";

static lv_obj_t *s_cross = NULL;     /* 跟手准星 */
static lv_obj_t *s_lbl_xy = NULL;    /* 顶部坐标 */
static lv_obj_t *s_lbl_hit = NULL;   /* 点中了哪个角 */

/* ---------------- 触摸自检界面 ---------------- */
static void scr_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t p;

    if (indev == NULL) return;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_obj_remove_flag(s_cross, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_cross, p.x - 22, p.y - 22);   /* 44x44，中心对准手指 */

        char buf[48];
        snprintf(buf, sizeof(buf), "x=%d  y=%d", (int)p.x, (int)p.y);
        lv_label_set_text(s_lbl_xy, buf);
    } else if (code == LV_EVENT_RELEASED) {
        lv_obj_add_flag(s_cross, LV_OBJ_FLAG_HIDDEN);
    }
}

static void corner_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *name = (const char *)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "corner hit: %s", name);
    lv_label_set_text(s_lbl_hit, name);
}

static void ui_touch_test(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d131b), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, scr_event_cb, LV_EVENT_ALL, NULL);

    /* 准星 */
    s_cross = lv_obj_create(scr);
    lv_obj_remove_style_all(s_cross);
    lv_obj_set_size(s_cross, 44, 44);
    lv_obj_set_style_border_color(s_cross, lv_color_hex(0xffd166), 0);
    lv_obj_set_style_border_width(s_cross, 2, 0);
    lv_obj_set_style_radius(s_cross, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_cross, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_cross, LV_OBJ_FLAG_HIDDEN);

    /* 顶部坐标 */
    s_lbl_xy = lv_label_create(scr);
    lv_label_set_text(s_lbl_xy, "TOUCH THE SCREEN");
    lv_obj_set_style_text_font(s_lbl_xy, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(s_lbl_xy, lv_color_hex(0x3ddc78), 0);
    lv_obj_align(s_lbl_xy, LV_ALIGN_TOP_MID, 0, 8);

    /* 中间提示 */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "点四个角");
    lv_obj_set_style_text_font(hint, &lv_font_misans_24, 0);
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, -40);

    s_lbl_hit = lv_label_create(scr);
    lv_label_set_text(s_lbl_hit, "----");
    lv_obj_set_style_text_font(s_lbl_hit, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(s_lbl_hit, lv_color_hex(0x7d8ea0), 0);
    lv_obj_align(s_lbl_hit, LV_ALIGN_CENTER, 0, 20);

    /* 四角目标框：点中会显示 TL/TR/BL/BR，用来核对坐标映射 */
    static const struct {
        const char *name;
        lv_align_t align;
        int dx, dy;
    } corners[] = {
        {"TL", LV_ALIGN_TOP_LEFT,     6,   6},
        {"TR", LV_ALIGN_TOP_RIGHT,   -6,   6},
        {"BL", LV_ALIGN_BOTTOM_LEFT,  6,  -6},
        {"BR", LV_ALIGN_BOTTOM_RIGHT, -6, -6},
    };
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        lv_obj_t *box = lv_button_create(scr);
        lv_obj_set_size(box, 64, 64);
        lv_obj_align(box, corners[i].align, corners[i].dx, corners[i].dy);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x185fa5), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_add_event_cb(box, corner_event_cb, LV_EVENT_CLICKED, (void *)corners[i].name);

        lv_obj_t *lbl = lv_label_create(box);
        lv_label_set_text(lbl, corners[i].name);
        lv_obj_set_style_text_font(lbl, &lv_font_misans_16, 0);
        lv_obj_center(lbl);
    }
}

static void heartbeat_task(void *arg)
{
    uint32_t n = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
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

    bsp_lvgl_lock();
    ui_touch_test();
    bsp_lvgl_unlock();

    bsp_lcd_set_backlight(80);
    ESP_LOGI(TAG, "M2 touch test screen shown");

    xTaskCreatePinnedToCore(heartbeat_task, "hb", 3072, NULL, 2, NULL, 0);
}
