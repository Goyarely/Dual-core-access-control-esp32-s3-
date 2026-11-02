#include "link_task.h"
#include "link_proto.h"
#include "app_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "link";

#define LINK_UART       UART_NUM_1
#define RX_BUF_SIZE     1024
#define ACK_TIMEOUT_MS  100
#define ACK_RETRY       3
#define ONLINE_TIMEOUT_MS 3000

static SemaphoreHandle_t s_tx_mutex;
static link_rx_t s_rx_win;

static volatile bool s_online;
static volatile int64_t s_last_rx_us;
static volatile uint32_t s_epoch;
static volatile bool s_wait_ack;
static volatile uint8_t s_wait_seq;
static volatile bool s_ack_ok;
static volatile uint8_t s_ack_result;
static volatile uint8_t s_ack_remain;
static uint8_t s_seq;

/* 自环测试用：记住最近发出去的 STATE 帧 seq */
static volatile bool s_loop_wait;
static volatile uint8_t s_loop_seq;
static volatile bool s_loop_got;

static uint8_t next_seq(void)
{
    return s_seq++;
}

static void tx_frame(uint8_t type, uint8_t seq, const void *payload, uint8_t len)
{
    uint8_t buf[LINK_MAX_FRAME];
    size_t n = link_encode(buf, type, seq, payload, len);
    if (n == 0) return;

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(LINK_UART, (const char *)buf, n);
    xSemaphoreGive(s_tx_mutex);
}

/* ---------------- 收帧处理 ---------------- */
static void handle_frame(const link_frame_t *f)
{
    s_last_rx_us = esp_timer_get_time();
    if (!s_online) {
        s_online = true;
        ESP_LOGI(TAG, "链路在线（收到对端帧）");
    }

    /* 去重窗口只保护 UNLOCK_REQ：心跳/ACK/状态帧本身幂等，
     * 而且对端重启后 seq 会回到 0，若一并去重会被窗口判成"倒退太远"，
     * 表现是明明通着却持续十几秒显示离线（2026-11-02 实测）。 */
    if (f->type == LINK_T_UNLOCK_REQ && !link_rx_accept(&s_rx_win, f->seq)) {
        ESP_LOGW(TAG, "重复的开锁请求 seq=%u，丢弃", f->seq);
        return;
    }
    ESP_LOGD(TAG, "RX type=%u seq=%u len=%u", f->type, f->seq, f->len);

    switch (f->type) {
    case LINK_T_HEARTBEAT: {
        link_heartbeat_t hb;
        if (link_unpack_heartbeat(f->payload, f->len, &hb)) s_epoch = hb.epoch;
        break;
    }
    case LINK_T_ACK:
        if (f->len >= 1 && s_wait_ack && f->payload[0] == s_wait_seq) s_ack_ok = true;
        break;
    case LINK_T_UNLOCK_ACK: {
        link_unlock_ack_t ack;
        if (link_unpack_unlock_ack(f->payload, f->len, &ack)) {
            s_ack_result = ack.result;
            s_ack_remain = ack.remain;
            if (s_wait_ack) s_ack_ok = true;
        }
        break;
    }
    case LINK_T_STATE:
        if (s_loop_wait && f->seq == s_loop_seq) s_loop_got = true;
        break;
    default:
        break;
    }

    /* 收到非 ACK 帧就回一个 ACK（协议要求） */
    if (f->type != LINK_T_ACK) {
        uint8_t acked = f->seq;
        tx_frame(LINK_T_ACK, next_seq(), &acked, 1);
    }
}

static void rx_task(void *arg)
{
    uint8_t buf[256];
    static uint8_t stream[LINK_MAX_FRAME * 2];
    size_t used = 0;

    while (1) {
        int n = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            if (used + n > sizeof(stream)) used = 0;    /* 溢出保护 */
            memcpy(stream + used, buf, n);
            used += n;

            size_t off = 0;
            while (used - off >= LINK_HDR_LEN) {
                link_frame_t f;
                size_t consume = 0;
                if (link_find_frame(stream + off, used - off, &f, &consume) && consume > 0) {
                    handle_frame(&f);
                    off += consume;
                } else {
                    break;
                }
            }
            if (off > 0) {
                memmove(stream, stream + off, used - off);
                used -= off;
            }
        }

        if (s_online && (esp_timer_get_time() - s_last_rx_us) > ONLINE_TIMEOUT_MS * 1000LL) {
            s_online = false;
            ESP_LOGW(TAG, "链路断开（%d ms 没收到对端任何帧）", ONLINE_TIMEOUT_MS);
        }
    }
}

/* 心跳：每秒一次，带 epoch 供对端校准时间 */
static void heartbeat_task(void *arg)
{
    while (1) {
        link_heartbeat_t hb = {
            /* M8 阶段先用开机秒数；M12 接 NTP 后换成真实 epoch */
            .epoch = (uint32_t)(esp_timer_get_time() / 1000000),
            .status = s_online ? 0x01 : 0x00,
        };
        uint8_t pl[8];
        size_t n = link_pack_heartbeat(pl, &hb);
        tx_frame(LINK_T_HEARTBEAT, next_seq(), pl, (uint8_t)n);
        if (!s_online) ESP_LOGW(TAG, "心跳已发，但对端仍无响应（%us）", (unsigned)hb.epoch);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------- 对外接口 ---------------- */
bool link_is_online(void)
{
    return s_online;
}

uint32_t link_epoch(void)
{
    return s_epoch;
}

esp_err_t link_loopback_test(int rounds)
{
    ESP_LOGW(TAG, "自环测试：请确认 IO%d(TX) 与 IO%d(RX) 已短接", LINK_TX_GPIO, LINK_RX_GPIO);

    int pass = 0;
    for (int i = 0; i < rounds; i++) {
        uint8_t payload[2] = {(uint8_t)i, 0x5A};
        uint8_t seq = next_seq();

        s_loop_seq = seq;
        s_loop_got = false;
        s_loop_wait = true;

        int64_t t0 = esp_timer_get_time();
        tx_frame(LINK_T_STATE, seq, payload, sizeof(payload));

        while (!s_loop_got && (esp_timer_get_time() - t0) < 500 * 1000LL) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        s_loop_wait = false;

        int64_t dt = (esp_timer_get_time() - t0) / 1000;
        if (s_loop_got) {
            pass++;
            ESP_LOGI(TAG, "自环 #%d OK  seq=%u  rtt=%lldms", i + 1, seq, (long long)dt);
        } else {
            ESP_LOGE(TAG, "自环 #%d 失败：500ms 内没收到自己发的帧"
                          "（检查 IO%d 与 IO%d 是否短接、是否共地）",
                     i + 1, LINK_TX_GPIO, LINK_RX_GPIO);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "自环结果：%d/%d 通过", pass, rounds);
    return (pass == rounds) ? ESP_OK : ESP_FAIL;
}

int link_unlock_request(uint8_t reason, uint8_t *remain)
{
    link_unlock_req_t req = {
        .timestamp = (uint32_t)(esp_timer_get_time() / 1000000),
        .nonce = (uint32_t)esp_timer_get_time(),
        .reason = reason,
    };
    uint8_t payload[16];
    size_t plen = link_pack_unlock_req(payload, &req);

    for (int attempt = 0; attempt < ACK_RETRY; attempt++) {
        uint8_t seq = next_seq();
        s_wait_seq = seq;
        s_ack_ok = false;
        s_wait_ack = true;

        tx_frame(LINK_T_UNLOCK_REQ, seq, payload, (uint8_t)plen);
        ESP_LOGI(TAG, "UNLOCK_REQ 发出 seq=%u 第 %d 次", seq, attempt + 1);

        int waited = 0;
        while (!s_ack_ok && waited < ACK_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        s_wait_ack = false;

        if (s_ack_ok) {
            if (remain) *remain = s_ack_remain;
            ESP_LOGI(TAG, "收到 UNLOCK_ACK result=%u remain=%u",
                     s_ack_result, s_ack_remain);
            return s_ack_result;
        }
        ESP_LOGW(TAG, "第 %d 次没等到 ACK，重传", attempt + 1);
    }

    ESP_LOGE(TAG, "开锁请求 %d 次全超时（链路不通？）", ACK_RETRY);
    return -1;
}

esp_err_t link_task_start(void)
{
    if (s_tx_mutex == NULL) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (s_tx_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    link_rx_init(&s_rx_win);

    uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART1 ready  TX=IO%d RX=IO%d  %d 8N1",
             LINK_TX_GPIO, LINK_RX_GPIO, LINK_BAUD);

    xTaskCreatePinnedToCore(rx_task, "link_rx", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(heartbeat_task, "link_hb", 3072, NULL, 3, NULL, 0);
    return ESP_OK;
}
