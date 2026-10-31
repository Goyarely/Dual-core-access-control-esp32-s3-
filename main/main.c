/*
 * door_access —— 双核智能门禁（ESP32-S3 侧：感知 / 交互 / 云）
 *
 * M0：工程骨架（编译 + 启动自检）
 * M1：屏 BSP —— ST7796U 320x480 竖屏 + LVGL 9，中文标题
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
#include "lv_fonts.h"

static const char *TAG = "door_access";

/* ---------------- 字库自检 ----------------
 * 用运行时查询代替"看屏幕猜"：能查到字形说明字库表本身没问题，
 * 查不到就是生成的字库和 LVGL 9 的结构不兼容。 */
static void font_selfcheck(void)
{
    struct {
        const char *name;
        const lv_font_t *font;
        uint32_t cp;
    } items[] = {
        {"misans_16 'A'", &lv_font_misans_16, 'A'},
        {"misans_16 '门'", &lv_font_misans_16, 0x95E8},
        {"misans_24 '门'", &lv_font_misans_24, 0x95E8},
        {"default 'A'", LV_FONT_DEFAULT, 'A'},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_font_glyph_dsc_t d = {0};
        bool ok = lv_font_get_glyph_dsc(items[i].font, &d, items[i].cp, 0);
        ESP_LOGI(TAG, "font %-16s ok=%d adv=%d box=%dx%d",
                 items[i].name, ok, d.adv_w, d.box_w, d.box_h);
    }
}

/* ---------------- 启动页 ---------------- */
static void ui_boot_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d131b), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 标题：中文能否显示，直接检验字库链接和编码 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "门禁系统");
    lv_obj_set_style_text_font(title, &lv_font_misans_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -48);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "door_access v" APP_VERSION);
    lv_obj_set_style_text_font(sub, &lv_font_misans_16, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7d8ea0), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 0);

    /* 对照组：默认字体的纯 ASCII，用来区分"字库坏了"还是"布局偏了" */
    lv_obj_t *ascii = lv_label_create(scr);
    lv_label_set_text(ascii, "DOOR ACCESS M1");
    lv_obj_set_style_text_color(ascii, lv_color_hex(0xff8a00), 0);
    lv_obj_align(ascii, LV_ALIGN_CENTER, 0, -96);

    /* 三色条：红绿蓝顺序错 = BGR/字节序问题，一眼看出来 */
    const uint32_t colors[3] = {0xff0000, 0x00ff00, 0x0000ff};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 72, 14);
        lv_obj_set_style_bg_color(bar, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_align(bar, LV_ALIGN_CENTER, (i - 1) * 84, 56);
    }

    /* 四角标记：检查画面有没有偏移/裁切 */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x3ddc78), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        int x = (i % 2) ? 1 : -1;
        int y = (i / 2) ? 1 : -1;
        lv_obj_align(dot, LV_ALIGN_CENTER, x * (LCD_H_RES / 2 - 12), y * (LCD_V_RES / 2 - 12));
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

    bsp_lvgl_lock();
    font_selfcheck();
    ui_boot_screen();
    bsp_lvgl_unlock();

    bsp_lcd_set_backlight(80);
    ESP_LOGI(TAG, "M1 boot screen shown, backlight 80%%");

    xTaskCreatePinnedToCore(heartbeat_task, "hb", 3072, NULL, 2, NULL, 0);
}
