#include "link_proto.h"
#include <string.h>

/* 两板必须烧同一份密钥；换了就两边一起换，否则 HMAC 永远不过 */
static const uint8_t LINK_MAC_KEY[16] = {
    0x44, 0x6f, 0x6f, 0x72, 0x41, 0x63, 0x63, 0x65,
    0x73, 0x73, 0x2d, 0x32, 0x30, 0x32, 0x36, 0x21,
};

/* ---------------- CRC16 / MODBUS ---------------- */
uint16_t link_crc16(const uint8_t *data, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ---------------- SipHash-2-4 → 32 位 ---------------- */
#define ROTL64(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND(v0, v1, v2, v3)      \
    do {                              \
        v0 += v1; v1 = ROTL64(v1, 13); v1 ^= v0; v0 = ROTL64(v0, 32); \
        v2 += v3; v3 = ROTL64(v3, 16); v3 ^= v2;                     \
        v0 += v3; v3 = ROTL64(v3, 21); v3 ^= v0;                     \
        v2 += v1; v1 = ROTL64(v1, 17); v1 ^= v2; v2 = ROTL64(v2, 32); \
    } while (0)

static uint64_t rd64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint64_t siphash24(const uint8_t *in, size_t len, const uint8_t key[16])
{
    uint64_t k0 = rd64le(key);
    uint64_t k1 = rd64le(key + 8);

    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;

    const size_t end = len - (len % 8);
    for (size_t i = 0; i < end; i += 8) {
        uint64_t m = rd64le(in + i);
        v3 ^= m;
        SIPROUND(v0, v1, v2, v3);
        SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }

    uint64_t b = (uint64_t)len << 56;
    for (size_t i = 0; i < (len & 7); i++) {
        b |= (uint64_t)in[end + i] << (8 * i);
    }
    v3 ^= b;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    v0 ^= b;

    v2 ^= 0xff;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

uint32_t link_mac32(const uint8_t *data, size_t n)
{
    uint64_t h = siphash24(data, n, LINK_MAC_KEY);
    /* 高低 32 位折叠，两侧都用到 */
    return (uint32_t)(h ^ (h >> 32));
}

/* ---------------- 编解码 ---------------- */
static void wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t link_encode(uint8_t *out, uint8_t type, uint8_t seq, const void *payload, uint8_t len)
{
    if (out == NULL || len > LINK_MAX_PAYLOAD) return 0;
    if (len > 0 && payload == NULL) return 0;

    out[0] = LINK_MAGIC0;
    out[1] = LINK_MAGIC1;
    out[2] = LINK_VER;
    out[3] = type;
    out[4] = seq;
    out[5] = len;
    if (len) memcpy(&out[LINK_HDR_LEN], payload, len);

    /* CRC 覆盖 ver..payload */
    uint16_t crc = link_crc16(&out[2], (size_t)(LINK_HDR_LEN - 2) + len);
    wr16le(&out[LINK_HDR_LEN + len], crc);

    /* MAC 覆盖 ver..CRC（连 CRC 一起签，改 CRC 也会被发现） */
    uint32_t mac = link_mac32(&out[2], (size_t)(LINK_HDR_LEN - 2) + len + LINK_CRC_LEN);
    wr32le(&out[LINK_HDR_LEN + len + LINK_CRC_LEN], mac);

    return (size_t)(LINK_HDR_LEN + len + LINK_CRC_LEN + LINK_MAC_LEN);
}

bool link_decode(const uint8_t *buf, size_t n, link_frame_t *out)
{
    if (buf == NULL || out == NULL) return false;
    if (n < LINK_HDR_LEN + LINK_CRC_LEN + LINK_MAC_LEN) return false;
    if (buf[0] != LINK_MAGIC0 || buf[1] != LINK_MAGIC1) return false;
    if (buf[2] != LINK_VER) return false;

    uint8_t len = buf[5];
    if (len > LINK_MAX_PAYLOAD) return false;
    if (n != (size_t)(LINK_HDR_LEN + len + LINK_CRC_LEN + LINK_MAC_LEN)) return false;

    uint16_t crc = rd16le(&buf[LINK_HDR_LEN + len]);
    if (crc != link_crc16(&buf[2], (size_t)(LINK_HDR_LEN - 2) + len)) return false;

    uint32_t mac = rd32le(&buf[LINK_HDR_LEN + len + LINK_CRC_LEN]);
    if (mac != link_mac32(&buf[2], (size_t)(LINK_HDR_LEN - 2) + len + LINK_CRC_LEN)) return false;

    out->type = buf[3];
    out->seq = buf[4];
    out->len = len;
    if (len) memcpy(out->payload, &buf[LINK_HDR_LEN], len);
    return true;
}

bool link_find_frame(const uint8_t *buf, size_t n, link_frame_t *out, size_t *used)
{
    if (buf == NULL || out == NULL || used == NULL) return false;

    size_t i = 0;
    while (i + 1 < n) {
        if (buf[i] != LINK_MAGIC0 || buf[i + 1] != LINK_MAGIC1) {
            i++;
            continue;
        }
        if (i + LINK_HDR_LEN > n) break;      /* 头还没收全 */

        uint8_t len = buf[i + 5];
        if (len > LINK_MAX_PAYLOAD) {          /* 假帧头，跳过 */
            i++;
            continue;
        }
        size_t total = (size_t)(LINK_HDR_LEN + len + LINK_CRC_LEN + LINK_MAC_LEN);
        if (i + total > n) break;              /* 帧还没收全 */

        if (link_decode(&buf[i], total, out)) {
            *used = i + total;
            return true;
        }
        i++;                                   /* 校验不过，继续找下一个帧头 */
    }
    *used = i;
    return false;
}

/* ---------------- 去重 ---------------- */
void link_rx_init(link_rx_t *rx)
{
    if (rx == NULL) return;
    rx->seen = 0;
    rx->last = 0;
    rx->inited = false;
}

bool link_rx_accept(link_rx_t *rx, uint8_t seq)
{
    if (rx == NULL) return false;

    if (!rx->inited) {
        rx->inited = true;
        rx->last = seq;
        rx->seen = 1;
        return true;
    }

    uint8_t diff = (uint8_t)(seq - rx->last);
    if (diff == 0) return false;               /* 同一个 seq：重传来的重复帧 */

    if (diff < 128) {
        /* 前进了 diff 格，位图左移 */
        rx->seen = (diff >= 32) ? 1u : ((rx->seen << diff) | 1u);
    } else {
        /* 落后（乱序/回绕）：查位图里对应位是否已收到 */
        uint8_t back = (uint8_t)(-diff);
        if (back >= 32) return false;
        uint32_t bit = 1u << back;
        if (rx->seen & bit) return false;
        rx->seen |= bit;
        return true;
    }
    rx->last = seq;
    return true;
}

/* ---------------- 载荷 ---------------- */
size_t link_pack_unlock_req(uint8_t *out, const link_unlock_req_t *req)
{
    if (out == NULL || req == NULL) return 0;
    wr32le(&out[0], req->timestamp);
    wr32le(&out[4], req->nonce);
    out[8] = req->reason;
    return 9;
}

bool link_unpack_unlock_req(const uint8_t *in, uint8_t len, link_unlock_req_t *req)
{
    if (in == NULL || req == NULL || len != 9) return false;
    req->timestamp = rd32le(&in[0]);
    req->nonce = rd32le(&in[4]);
    req->reason = in[8];
    return true;
}

size_t link_pack_unlock_ack(uint8_t *out, const link_unlock_ack_t *ack)
{
    if (out == NULL || ack == NULL) return 0;
    out[0] = ack->result;
    out[1] = ack->remain;
    return 2;
}

bool link_unpack_unlock_ack(const uint8_t *in, uint8_t len, link_unlock_ack_t *ack)
{
    if (in == NULL || ack == NULL || len != 2) return false;
    ack->result = in[0];
    ack->remain = in[1];
    return true;
}

size_t link_pack_heartbeat(uint8_t *out, const link_heartbeat_t *hb)
{
    if (out == NULL || hb == NULL) return 0;
    wr32le(&out[0], hb->epoch);
    out[4] = hb->status;
    return 5;
}

bool link_unpack_heartbeat(const uint8_t *in, uint8_t len, link_heartbeat_t *hb)
{
    if (in == NULL || hb == NULL || len != 5) return false;
    hb->epoch = rd32le(&in[0]);
    hb->status = in[4];
    return true;
}
