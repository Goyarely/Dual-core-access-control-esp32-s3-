#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * link_task —— ESP32 侧的 UART1 链路（IO10=TX / IO11=RX，115200 8N1）
 *
 * 分工：
 *   link_proto  只管帧的编解码（纯函数，两板同源）
 *   link_task   管串口、心跳、ACK 等待与重传
 *
 * 注意 IO10 的三重占用：板载 LED6 / 触摸 TP-INT / 现在归 UART1_TX。
 * 触摸已在 M2 用 G_MODE=0 切轮询让出这根脚。
 */

esp_err_t link_task_start(void);

/** 3 秒内收到过对端任何帧 = 在线 */
bool link_is_online(void);

/** 最近一次心跳带来的 epoch（对端时间基准，秒） */
uint32_t link_epoch(void);

/**
 * @brief 发一次开锁请求并等 UNLOCK_ACK（100ms 超时 × 3 次重传）
 * @param reason LINK_REASON_FACE / PASSWORD / ADMIN
 * @param remain 回填剩余尝试次数（可能为 NULL）
 * @return link_result_t；超时返回 -1
 */
int link_unlock_request(uint8_t reason, uint8_t *remain);

/** 自环测试：发一帧，看能不能原样收回来（用于 IO10-IO11 短接验证） */
esp_err_t link_loopback_test(int rounds);
