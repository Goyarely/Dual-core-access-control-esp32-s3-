"""link_proto 的 Python 同源实现（与 components/link/link_proto.c 逐字节一致）。

帧格式（小端）：
    AA55 | ver | type | seq | len | payload | CRC16 | MAC32
     2B     1B    1B     1B    1B    0~64B      2B      4B

    CRC16(MODBUS) 覆盖 ver..payload
    MAC32(SipHash-2-4 折叠) 覆盖 ver..CRC
"""

from __future__ import annotations

import struct

MAGIC0, MAGIC1 = 0xAA, 0x55
VER = 0x01
MAX_PAYLOAD = 64
HDR_LEN = 6
CRC_LEN = 2
MAC_LEN = 4
MAX_FRAME = HDR_LEN + MAX_PAYLOAD + CRC_LEN + MAC_LEN

# 必须与 link_proto.c 里的 LINK_MAC_KEY 完全一致
MAC_KEY = bytes([0x44, 0x6f, 0x6f, 0x72, 0x41, 0x63, 0x63, 0x65,
                 0x73, 0x73, 0x2d, 0x32, 0x30, 0x32, 0x36, 0x21])

# 消息类型
T_HEARTBEAT = 1
T_ACK = 2
T_UNLOCK_REQ = 3
T_UNLOCK_ACK = 4
T_KEY_EVT = 5
T_STATE = 6

TYPE_NAME = {
    T_HEARTBEAT: "HEARTBEAT",
    T_ACK: "ACK",
    T_UNLOCK_REQ: "UNLOCK_REQ",
    T_UNLOCK_ACK: "UNLOCK_ACK",
    T_KEY_EVT: "KEY_EVT",
    T_STATE: "STATE",
}

RESULT_NAME = {0: "OK", 1: "DENIED", 2: "REPLAY", 3: "LOCKED", 4: "BAD_FRAME"}

MASK64 = (1 << 64) - 1


# ---------------------------------------------------------------- CRC16
def crc16(data: bytes) -> int:
    """CRC-16/MODBUS，初值 0xFFFF，多项式 0xA001（反射）"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


# --------------------------------------------------------------- SipHash
def _rotl64(x: int, b: int) -> int:
    return ((x << b) | (x >> (64 - b))) & MASK64


def siphash24(data: bytes, key: bytes) -> int:
    k0, k1 = struct.unpack("<QQ", key[:16])
    v0 = 0x736F6D6570736575 ^ k0
    v1 = 0x646F72616E646F6D ^ k1
    v2 = 0x6C7967656E657261 ^ k0
    v3 = 0x7465646279746573 ^ k1

    def rnd(v0, v1, v2, v3):
        v0 = (v0 + v1) & MASK64
        v1 = _rotl64(v1, 13) ^ v0
        v0 = _rotl64(v0, 32)
        v2 = (v2 + v3) & MASK64
        v3 = _rotl64(v3, 16) ^ v2
        v0 = (v0 + v3) & MASK64
        v3 = _rotl64(v3, 21) ^ v0
        v2 = (v2 + v1) & MASK64
        v1 = _rotl64(v1, 17) ^ v2
        v2 = _rotl64(v2, 32)
        return v0, v1, v2, v3

    n = len(data)
    end = n - (n % 8)
    for i in range(0, end, 8):
        m = struct.unpack_from("<Q", data, i)[0]
        v3 ^= m
        v0, v1, v2, v3 = rnd(v0, v1, v2, v3)
        v0, v1, v2, v3 = rnd(v0, v1, v2, v3)
        v0 ^= m

    b = (n & 0xFF) << 56
    for i in range(n & 7):
        b |= data[end + i] << (8 * i)
    v3 ^= b
    v0, v1, v2, v3 = rnd(v0, v1, v2, v3)
    v0, v1, v2, v3 = rnd(v0, v1, v2, v3)
    v0 ^= b

    v2 ^= 0xFF
    for _ in range(4):
        v0, v1, v2, v3 = rnd(v0, v1, v2, v3)

    return (v0 ^ v1 ^ v2 ^ v3) & MASK64


def mac32(data: bytes) -> int:
    h = siphash24(data, MAC_KEY)
    return (h ^ (h >> 32)) & 0xFFFFFFFF


# --------------------------------------------------------------- 编解码
def encode(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    body = bytes([VER, msg_type & 0xFF, seq & 0xFF, len(payload)]) + payload
    crc = crc16(body)
    mac = mac32(body + struct.pack("<H", crc))
    return bytes([MAGIC0, MAGIC1]) + body + struct.pack("<HI", crc, mac)


class Frame:
    __slots__ = ("type", "seq", "payload", "raw")

    def __init__(self, msg_type, seq, payload, raw):
        self.type = msg_type
        self.seq = seq
        self.payload = payload
        self.raw = raw

    @property
    def name(self) -> str:
        return TYPE_NAME.get(self.type, f"TYPE_{self.type}")

    def __repr__(self) -> str:
        return f"<{self.name} seq={self.seq} len={len(self.payload)}>"


def decode(buf: bytes) -> Frame | None:
    if len(buf) < HDR_LEN + CRC_LEN + MAC_LEN:
        return None
    if buf[0] != MAGIC0 or buf[1] != MAGIC1 or buf[2] != VER:
        return None
    length = buf[5]
    if length > MAX_PAYLOAD:
        return None
    if len(buf) != HDR_LEN + length + CRC_LEN + MAC_LEN:
        return None

    body = buf[2:HDR_LEN + length]
    crc = struct.unpack_from("<H", buf, HDR_LEN + length)[0]
    if crc != crc16(body):
        return None
    mac = struct.unpack_from("<I", buf, HDR_LEN + length + CRC_LEN)[0]
    if mac != mac32(body + struct.pack("<H", crc)):
        return None

    return Frame(buf[3], buf[4], buf[HDR_LEN:HDR_LEN + length], bytes(buf))


def find_frame(stream: bytes) -> tuple[Frame | None, int]:
    """从字节流里找一帧，返回 (frame, 消耗字节数)。找不到时消耗掉无效前导字节。"""
    i = 0
    n = len(stream)
    while i + 1 < n:
        if stream[i] != MAGIC0 or stream[i + 1] != MAGIC1:
            i += 1
            continue
        if i + HDR_LEN > n:
            break
        length = stream[i + 5]
        if length > MAX_PAYLOAD:
            i += 1
            continue
        total = HDR_LEN + length + CRC_LEN + MAC_LEN
        if i + total > n:
            break
        f = decode(stream[i:i + total])
        if f is not None:
            return f, i + total
        i += 1
    return None, i


# --------------------------------------------------------------- 载荷
def pack_unlock_req(timestamp: int, nonce: int, reason: int) -> bytes:
    return struct.pack("<IIB", timestamp, nonce, reason)


def unpack_unlock_req(payload: bytes) -> tuple[int, int, int]:
    if len(payload) != 9:
        raise ValueError("bad UNLOCK_REQ length")
    return struct.unpack("<IIB", payload)


def pack_unlock_ack(result: int, remain: int) -> bytes:
    return struct.pack("<BB", result, remain)


def unpack_unlock_ack(payload: bytes) -> tuple[int, int]:
    if len(payload) != 2:
        raise ValueError("bad UNLOCK_ACK length")
    return struct.unpack("<BB", payload)


def pack_heartbeat(epoch: int, status: int) -> bytes:
    return struct.pack("<IB", epoch, status)


def unpack_heartbeat(payload: bytes) -> tuple[int, int]:
    if len(payload) != 5:
        raise ValueError("bad HEARTBEAT length")
    return struct.unpack("<IB", payload)


# --------------------------------------------------------------- 去重窗口
class RxWindow:
    """与 C 版 link_rx_accept 完全一致的镜像（用于交叉验证算法）"""

    def __init__(self) -> None:
        self.seen = 0
        self.last = 0
        self.inited = False

    def accept(self, seq: int) -> bool:
        seq &= 0xFF
        if not self.inited:
            self.inited = True
            self.last = seq
            self.seen = 1
            return True

        diff = (seq - self.last) & 0xFF
        if diff == 0:
            return False

        if diff < 128:
            self.seen = 1 if diff >= 32 else (((self.seen << diff) | 1) & 0xFFFFFFFF)
        else:
            back = (-diff) & 0xFF
            if back >= 32:
                return False
            bit = 1 << back
            if self.seen & bit:
                return False
            self.seen |= bit
            return True

        self.last = seq
        return True
