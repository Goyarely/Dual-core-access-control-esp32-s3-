#include "app_settings.h"
#include "app_config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "settings";
#define SETTINGS_FILE "/storage/settings.bin"

static app_settings_t s_cfg;
static bool s_loaded = false;

void app_settings_reset_default(void)
{
    s_cfg.sim_thr = FACE_SIM_THRESHOLD;
    s_cfg.fail_max = FAIL_MAX;
    s_cfg.lockout_sec = LOCKOUT_SEC;
    s_cfg.live_mode = LIVE_SUSPICIOUS;   /* 默认只在可疑时要求转头 */
}

app_settings_t *app_settings(void)
{
    if (!s_loaded) {
        s_loaded = true;
        app_settings_reset_default();

        FILE *f = fopen(SETTINGS_FILE, "rb");
        if (f) {
            app_settings_t t = {0};
            if (fread(&t, sizeof(t), 1, f) == 1 &&
                t.sim_thr > 0.1f && t.sim_thr < 0.99f &&
                t.fail_max >= 1 && t.fail_max <= 20 &&
                t.lockout_sec >= 5 && t.lockout_sec <= 600 &&
                t.live_mode >= 0 && t.live_mode <= 2) {
                s_cfg = t;
                ESP_LOGI(TAG, "load: thr=%.2f fail=%d lock=%ds live=%d",
                         s_cfg.sim_thr, s_cfg.fail_max, s_cfg.lockout_sec, s_cfg.live_mode);
            } else {
                ESP_LOGW(TAG, "settings.bin 内容异常，用默认值");
            }
            fclose(f);
        } else {
            ESP_LOGI(TAG, "no settings.bin, use defaults");
        }
    }
    return &s_cfg;
}

esp_err_t app_settings_save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", SETTINGS_FILE);
        return ESP_FAIL;
    }
    size_t n = fwrite(&s_cfg, sizeof(s_cfg), 1, f);
    fclose(f);
    ESP_LOGI(TAG, "save: thr=%.2f fail=%d lock=%ds",
             s_cfg.sim_thr, s_cfg.fail_max, s_cfg.lockout_sec);
    return (n == 1) ? ESP_OK : ESP_FAIL;
}
