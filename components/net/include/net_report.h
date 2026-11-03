#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

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
 * @param mid     成员在库里的 id（0 = 未识别/管理员），后端据此关联姓名
 * @param member  设备侧文字（如 "成员 3"），后端没设姓名时兜底显示
 * @param result  "OK" / "DENIED" / "REPLAY" / "LOCKED"
 * @param reason  "face" / "password" / "admin"
 * @param sim     相似度（0~1，没有就传 0）
 */
void net_report_push(uint16_t mid, const char *member, const char *result,
                     const char *reason, float sim);

/**
 * @brief 查成员姓名（来自后端人员表，未知返回 NULL）
 *
 * 姓名以后端为唯一来源：设备侧不存名字，开机/每分钟拉一次 /api/members。
 * 后端没填名字时返回 NULL，调用方回退成"成员 N"。
 */
const char *net_member_name(uint16_t mid);

/** 立即拉取一次人员表（连上 WiFi 后会自动调用） */
void net_members_refresh(void);
