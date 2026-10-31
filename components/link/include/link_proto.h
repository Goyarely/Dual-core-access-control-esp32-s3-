#pragma once
/*
 * link_proto —— 双板 UART 帧协议（ESP32 / GD32 同源实现，逐字节一致）
 *
 * 帧格式（小端）：
 *   ┌──────┬─────┬──────┬─────┬─────┬───────────┬───────┬────────┐
 *   │ AA55 │ ver │ type │ seq │ len │  payload  │ CRC16 │ MAC32  │
 *   │  2B  │ 1B  │  1B  │ 1B  │ 1B  │ 0~64B     │  2B   │  4B    │
 *   └──────┴─────┴──────┴─────┴─────┴───────────┴───────┴────────┘
 *
 * 三个校验各管一件事，别混：
 *   CRC16(MODBUS) 覆盖 ver..payload —— 只管"传输有没有串码"，不保证真实性
 *   MAC32(SipHash) 覆盖 ver..CRC    —— 带密钥，防伪造（CRC 对得上也过不了这关）
 *   时间戳 + nonce                 —— 防重放（应用层校验，见 link_rx_accept）
 *
 * ⚠ 本库是纯函数：不依赖 FreeRTOS、不碰 UART，两板各编译一份。
 *   ESP32 侧由 link_task 调，GD32 侧由 USART1 解析状态机调。
 *
 * 关于 MAC 的选型：总览里写的是 HMAC-SHA256 截断 4 字节，但 GD32 没有加密库，
 * 要额外移植一份 SHA256。这里改用 SipHash-2-4（短消息认证的标准算法，
 * 60 行可移植 C，Python 能逐位复现），同样带密钥、同样抗伪造，代码量小得多。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_MAGIC0        0xAA
#define LINK_MAGIC1        0x55
#define LINK_VER           0x01
#define LINK_MAX_PAYLOAD   64
#define LINK_HDR_LEN       6
#define LINK_CRC_LEN       2
#define LINK_MAC_LEN       4
#define LINK_MAX_FRAME     (LINK_HDR_LEN + LINK_MAX_PAYLOAD + LINK_CRC_LEN + LINK_MAC_LEN)

/* 消息类型 */
typedef enum {
    LINK_T_HEARTBEAT  = 1,   /* 双向：epoch(4) + 状态位(1) */
    LINK_T_ACK        = 2,   /* 双向：被确认帧的 seq(1) */
    LINK_T_UNLOCK_REQ = 3,   /* ESP32→GD32：时间戳(4) + nonce(4) + 原因(1) */
    LINK_T_UNLOCK_ACK = 4,   /* GD32→ESP32：结果(1) + 剩余尝试(1) */
    LINK_T_KEY_EVT    = 5,   /* GD32→ESP32：键值(1) + 按下/松开(1) */
    LINK_T_STATE      = 6,   /* 双向：门状态(1) + 锁定剩余(2) */
} link_type_t;

/* UNLOCK_ACK 结果码 */
typedef enum {
    LINK_RESULT_OK        = 0,
    LINK_RESULT_DENIED    = 1,   /* 状态机拒绝 */
    LINK_RESULT_REPLAY    = 2,   /* 时间戳/nonce 不合格 */
    LINK_RESULT_LOCKED    = 3,   /* 锁定中 */
    LINK_RESULT_BAD_FRAME = 4,   /* CRC/MAC 不过 */
} link_result_t;

/* 开锁原因 */
typedef enum {
    LINK_REASON_FACE    = 0,
    LINK_REASON_PASSWORD = 1,
    LINK_REASON_ADMIN   = 2,
} link_reason_t;

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[LINK_MAX_PAYLOAD];
} link_frame_t;

/* ---------------- 编解码 ---------------- */
uint16_t link_crc16(const uint8_t *data, size_t n);
uint32_t link_mac32(const uint8_t *data, size_t n);

/**
 * @brief 组一帧
 * @return 帧总长度；0 表示参数非法（len > LINK_MAX_PAYLOAD）
 */
size_t link_encode(uint8_t *out, uint8_t type, uint8_t seq, const void *payload, uint8_t len);

/**
 * @brief 校验并解一帧（magic / 长度 / CRC / MAC 全过才返回 true）
 * @param n 传入的字节数，必须正好是一帧
 */
bool link_decode(const uint8_t *buf, size_t n, link_frame_t *out);

/**
 * @brief 从字节流里找一个完整帧（用于串口粘包）
 * @param used 返回消耗掉的字节数（找不到时会把无效前导字节吃掉）
 * @return true = out 里有完整且校验通过的帧
 */
bool link_find_frame(const uint8_t *buf, size_t n, link_frame_t *out, size_t *used);

/* ---------------- 去重窗口 ---------------- */
typedef struct {
    uint32_t seen;      /* 最近 32 个 seq 的位图 */
    uint8_t  last;
    bool     inited;
} link_rx_t;

void link_rx_init(link_rx_t *rx);

/**
 * @brief 收帧去重：同一个 seq 第二次来返回 false（重传导致的重复帧要丢弃）
 */
bool link_rx_accept(link_rx_t *rx, uint8_t seq);

/* ---------------- 载荷小工具（小端） ---------------- */
typedef struct {
    uint32_t timestamp;
    uint32_t nonce;
    uint8_t  reason;
} link_unlock_req_t;

typedef struct {
    uint8_t result;
    uint8_t remain;
} link_unlock_ack_t;

typedef struct {
    uint32_t epoch;
    uint8_t  status;
} link_heartbeat_t;

size_t link_pack_unlock_req(uint8_t *out, const link_unlock_req_t *req);
bool   link_unpack_unlock_req(const uint8_t *in, uint8_t len, link_unlock_req_t *req);
size_t link_pack_unlock_ack(uint8_t *out, const link_unlock_ack_t *ack);
bool   link_unpack_unlock_ack(const uint8_t *in, uint8_t len, link_unlock_ack_t *ack);
size_t link_pack_heartbeat(uint8_t *out, const link_heartbeat_t *hb);
bool   link_unpack_heartbeat(const uint8_t *in, uint8_t len, link_heartbeat_t *hb);

#ifdef __cplusplus
}
#endif
