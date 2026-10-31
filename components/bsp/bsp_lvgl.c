#include "bsp_lvgl.h"
#include "bsp_lcd.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "app_config.h"

static const char *TAG = "bsp_lvgl";
static lv_display_t *s_disp = NULL;

esp_err_t bsp_lvgl_init(void)
{
    ESP_RETURN_ON_FALSE(s_disp == NULL, ESP_OK, TAG, "already init");

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    /* 缓冲放内部 RAM（PSRAM 不能被 SPI DMA 直接读），40 行一条 */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = bsp_lcd_io(),
        .panel_handle = bsp_lcd_panel(),
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        /* ⚠ 这里是面板方向的**唯一权威**：esp_lvgl_port 在 add_disp 时会用这组值
         * 重新调用 esp_lcd_panel_swap_xy/mirror，把 bsp_lcd_init 里设过的覆盖掉。
         * 本板实测：MX=0 时整屏左右镜像（文字反向、色块顺序反），所以必须 mirror_x = true。
         * 竖屏原生方向，不 swap。 */
        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .buff_spiram = false, .swap_bytes = 1},
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "add disp");

    ESP_LOGI(TAG, "LVGL %d.%d display %dx%d ready", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

lv_display_t *bsp_lvgl_display(void)
{
    return s_disp;
}

esp_err_t bsp_lvgl_lock(void)
{
    return lvgl_port_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT;   /* -1 = 一直等 */
}

void bsp_lvgl_unlock(void)
{
    lvgl_port_unlock();
}
