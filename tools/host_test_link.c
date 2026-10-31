/* 在 PC 上编译 link_proto.c，输出固定向量供 test_proto.py 与 Python 实现比对。
 * 用法：
 *   gcc -I components/link/include tools/host_test_link.c \
 *       components/link/link_proto.c -o /tmp/host_test_link
 *   /tmp/host_test_link
 */
#include <stdio.h>
#include <string.h>
#include "link_proto.h"

static void dump(const char *tag, const uint8_t *p, size_t n)
{
    printf("%s=", tag);
    for (size_t i = 0; i < n; i++) printf("%02X", p[i]);
    printf("\n");
}

int main(void)
{
    /* 1) CRC 标准校验值 */
    const char *v = "123456789";
    printf("crc=%04X\n", link_crc16((const uint8_t *)v, strlen(v)));

    /* 2) MAC 固定输入 */
    uint8_t macbuf[32];
    for (int i = 0; i < 32; i++) macbuf[i] = (uint8_t)i;
    printf("mac=%08X\n", link_mac32(macbuf, sizeof(macbuf)));

    /* 3) 完整 UNLOCK_REQ 帧 */
    link_unlock_req_t req = {.timestamp = 0x11223344u, .nonce = 0xDEADBEEFu, .reason = LINK_REASON_FACE};
    uint8_t pl[16];
    size_t pln = link_pack_unlock_req(pl, &req);
    uint8_t frame[LINK_MAX_FRAME];
    size_t fn = link_encode(frame, LINK_T_UNLOCK_REQ, 7, pl, (uint8_t)pln);
    dump("frame", frame, fn);

    /* 4) 去重窗口：固定 seq 序列，输出 1/0 */
    link_rx_t rx;
    link_rx_init(&rx);
    const uint8_t seqs[] = {0, 1, 2, 2, 5, 4, 3, 100, 101, 255, 1, 0};
    printf("dedup=");
    for (size_t i = 0; i < sizeof(seqs); i++) {
        printf("%d", link_rx_accept(&rx, seqs[i]) ? 1 : 0);
    }
    printf("\n");
    return 0;
}
