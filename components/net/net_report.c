#include "net_report.h"
#include "app_config.h"
#include "wifi_creds.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_client.h"

static const char *TAG = "net";

#define QUEUE_LEN      8
#define POST_RETRY     3
#define HTTP_TIMEOUT_MS 3000
#define MEMBER_LEN     16
#define RESULT_LEN     8
#define REASON_LEN     12

typedef struct {
    char  member[MEMBER_LEN];
    char  result[RESULT_LEN];
    char  reason[REASON_LEN];
    float sim;
    uint32_t ts;
} rec_t;

static QueueHandle_t s_q;
static volatile bool s_wifi_up;

/* ---------------- WiFi ---------------- */
static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        ESP_LOGW(TAG, "WiFi 断开，重连中");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_wifi_up = true;
        ESP_LOGI(TAG, "WiFi 已连接，IP=" IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static esp_err_t wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "连接 WiFi: %s", WIFI_SSID);
    return ESP_OK;
}

/* ---------------- 上报 ---------------- */
static bool http_post(const rec_t *r)
{
    char body[160];
    snprintf(body, sizeof(body),
             "{\"ts\":%lu,\"member\":\"%s\",\"result\":\"%s\",\"reason\":\"%s\",\"sim\":%.2f}",
             (unsigned long)r->ts, r->member, r->result, r->reason, r->sim);

    esp_http_client_config_t cfg = {
        .url = BACKEND_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return false;

    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "上报失败 err=%s status=%d", esp_err_to_name(err), status);
        return false;
    }
    return true;
}

static void report_task(void *arg)
{
    rec_t r;
    while (1) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;

        if (!s_wifi_up) {
            ESP_LOGW(TAG, "WiFi 未连接，丢弃记录 %s/%s", r.member, r.result);
            continue;
        }
        for (int i = 0; i < POST_RETRY; i++) {
            if (http_post(&r)) {
                ESP_LOGI(TAG, "已上报 %s %s (%.2f)", r.member, r.result, r.sim);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

esp_err_t net_report_start(void)
{
    s_q = xQueueCreate(QUEUE_LEN, sizeof(rec_t));
    if (s_q == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = wifi_start();
    if (err != ESP_OK) return err;

    xTaskCreatePinnedToCore(report_task, "net_report", 6144, NULL, 2, NULL, 0);
    return ESP_OK;
}

bool net_report_online(void)
{
    return s_wifi_up;
}

void net_report_push(const char *member, const char *result, const char *reason, float sim)
{
    if (s_q == NULL) return;

    rec_t r = {0};
    strncpy(r.member, member ? member : "-", MEMBER_LEN - 1);
    strncpy(r.result, result ? result : "-", RESULT_LEN - 1);
    strncpy(r.reason, reason ? reason : "-", REASON_LEN - 1);
    r.sim = sim;
    r.ts = (uint32_t)(esp_timer_get_time() / 1000000);

    if (xQueueSend(s_q, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "上报队列满，丢弃 %s", r.member);
    }
}
