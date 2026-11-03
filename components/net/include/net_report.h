#pragma once
#include "esp_err.h"
#include <stdbool.h>

/*
 * net_report —— 通行记录上报（WiFi + HTTP POST）
 *
 * 设计取舍：只发不收、异步、失败不阻塞主流程。
 * 门禁的核心是"开门"，网络只是记录旁路——WiFi 没连上时门禁必须照常工作，
 * 所以这里用队列 + 独立任务，POST 失败重试 3 次后丢弃，绝不让网络拖慢识别。
 */

/** 启动 WiFi（STA）与上报任务。不阻塞：连不上也立即返回，后台一直重试 */
esp_err_t net_report_start(void);

/** WiFi 是否已连上 */
bool net_report_online(void);

/**
 * @brief 投递一条通行记录（非阻塞，队列满则丢弃并打印警告）
 * @param member  成员名（如 Person_1），拒绝时传 "陌生人"
 * @param result  "OK" / "DENIED" / "REPLAY" / "LOCKED"
 * @param reason  "face" / "password" / "admin"
 * @param sim     相似度（0~1，没有就传 0）
 */
void net_report_push(const char *member, const char *result, const char *reason, float sim);
