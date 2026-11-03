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
    uint16_t mid;
    char  member[MEMBER_LEN];
    char  result[RESULT_LEN];
    char  reason[REASON_LEN];
    float sim;
    uint32_t ts;
} rec_t;

static QueueHandle_t s_q;
static volatile bool s_wifi_up;

/* 人员表缓存：mid -> 姓名（来自后端，掉电重拉） */
#define MEMBER_NAME_MAX  64
#define MEMBER_NAME_LEN  24
static uint16_t s_name_ids[MEMBER_NAME_MAX];
static char     s_names[MEMBER_NAME_MAX][MEMBER_NAME_LEN];
static int      s_name_cnt;
static int      s_members_ver = -1;     /* -1 = 还没拉过，首次必全量 */

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
        net_members_refresh();
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
             "{\"ts\":%lu,\"mid\":%u,\"member\":\"%s\",\"result\":\"%s\","
             "\"reason\":\"%s\",\"sim\":%.2f}",
             (unsigned long)r->ts, (unsigned)r->mid, r->member, r->result, r->reason, r->sim);

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

/* ---------------- 人员表 ---------------- */
void net_members_refresh(void)
{
    if (!s_wifi_up) return;

    esp_http_client_config_t cfg = {
        .url = BACKEND_MEMBERS_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return;

    static char body[2048];
    int len = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        len = esp_http_client_read(c, body, sizeof(body) - 1);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (len <= 0) {
        ESP_LOGW(TAG, "拉取人员表失败");
        return;
    }
    body[len] = '\0';

    /* 极简解析：服务端返回的是我们自己生成的数组
     *   [{"mid": 3, "name": "张三", "dept": "...", "visits": 1}, ...]
     * 只需要逐条找 "mid":<数字> 和 "name":"<字符串>"，不引入 JSON 库。 */
    int n = 0;
    const char *p = body;
    while (n < MEMBER_NAME_MAX) {
        const char *pm = strstr(p, "\"mid\"");
        if (pm == NULL) break;
        const char *colon = strchr(pm, ':');
        if (colon == NULL) break;
        uint16_t mid = (uint16_t)atoi(colon + 1);

        const char *pn = strstr(p, "\"name\"");
        if (pn == NULL) break;
        const char *q1 = strchr(pn, ':');          /* 冒号 */
        if (q1 == NULL) break;
        q1 = strchr(q1, '"');                      /* 值的起始引号 */
        if (q1 == NULL) break;
        q1++;
        const char *q2 = strchr(q1, '"');
        if (q2 == NULL) break;

        size_t len = (size_t)(q2 - q1);
        if (len > 0 && len < MEMBER_NAME_LEN) {
            s_name_ids[n] = mid;
            memcpy(s_names[n], q1, len);
            s_names[n][len] = '\0';
            n++;
        }
        p = q2 + 1;
    }
    s_name_cnt = n;
    ESP_LOGI(TAG, "人员表已更新，%d 条", s_name_cnt);
}

/* 轮询版本号：改动一次才全量拉人员表。失败返回 -1（保持旧缓存，静默降级）。 */
static int members_ver_fetch(void)
{
    if (!s_wifi_up) return -1;

    esp_http_client_config_t cfg = {
        .url = BACKEND_MEMBERS_VER_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return -1;

    static char body[64];
    int len = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        len = esp_http_client_read(c, body, sizeof(body) - 1);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (len <= 0) return -1;
    body[len] = '\0';

    const char *p = strstr(body, "\"ver\"");
    if (p == NULL) return -1;
    return atoi(strchr(p, ':') + 1);
}

const char *net_member_name(uint16_t mid)
{
    for (int i = 0; i < s_name_cnt; i++) {
        if (s_name_ids[i] == mid) return s_names[i];
    }
    return NULL;
}

static void report_task(void *arg)
{
    rec_t r;
    TickType_t last_ver = 0;
    while (1) {
        /* 版本轮询：每 10 秒只拉一个数字（~20 字节），改名后 10 秒内屏幕生效。
         * 之前是每 60 秒全量拉人员表，改名最长要等 1 分钟（2026-11-03 实测）。 */
        if (s_wifi_up && (xTaskGetTickCount() - last_ver) > pdMS_TO_TICKS(10000)) {
            last_ver = xTaskGetTickCount();
            int ver = members_ver_fetch();
            if (ver >= 0 && ver != s_members_ver) {
                ESP_LOGI(TAG, "人员表版本 %d -> %d，刷新", s_members_ver, ver);
                net_members_refresh();
                s_members_ver = ver;
            }
        }

        if (xQueueReceive(s_q, &r, pdMS_TO_TICKS(1000)) != pdTRUE) continue;

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

void net_report_push(uint16_t mid, const char *member, const char *result,
                     const char *reason, float sim)
{
    if (s_q == NULL) return;

    rec_t r = {0};
    r.mid = mid;
    strncpy(r.member, member ? member : "-", MEMBER_LEN - 1);
    strncpy(r.result, result ? result : "-", RESULT_LEN - 1);
    strncpy(r.reason, reason ? reason : "-", REASON_LEN - 1);
    r.sim = sim;
    r.ts = (uint32_t)(esp_timer_get_time() / 1000000);

    if (xQueueSend(s_q, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "上报队列满，丢弃 %s", r.member);
    }
}
