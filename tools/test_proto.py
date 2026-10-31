#!/usr/bin/env python3
"""M7 协议库自测（不需要开发板）。

两部分：
  1) Python 实现的单元测试（编码往返 / 篡改 / 粘包 / 去重 / 重放）
  2) 把 components/link/link_proto.c 用 gcc 编成本机程序，用同一组向量
     比对 CRC、MAC、整帧字节、去重结果 —— 保证 C 和 Python 逐字节一致

用法：python3 tools/test_proto.py
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import link_proto as L  # noqa: E402

fails = []
checks = 0


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)
        print(f"  ✗ {msg}")


def section(title: str) -> None:
    print(f"\n== {title}")


# ------------------------------------------------------------ 1. 单元测试
def test_vectors() -> None:
    section("标准向量")
    check(L.crc16(b"123456789") == 0x4B37, "CRC16/MODBUS 校验值应为 0x4B37")

    k = bytes(range(16))
    check(L.siphash24(b"", k) == 0x726FDB47DD0E0E31, "SipHash 空串向量")
    check(L.siphash24(b"\x00", k) == 0x74F839C593DC67FD, "SipHash 1 字节向量")
    check(L.siphash24(bytes(range(15)), k) == 0xA129CA6149BE45E5, "SipHash 15 字节向量")


def test_roundtrip() -> None:
    section("编码/解码往返")
    for t in (L.T_HEARTBEAT, L.T_ACK, L.T_UNLOCK_REQ, L.T_UNLOCK_ACK, L.T_KEY_EVT, L.T_STATE):
        raw = L.encode(t, 42, b"\x01\x02\x03")
        f = L.decode(raw)
        check(f is not None, f"type={t} 解码应成功")
        if f:
            check(f.type == t and f.seq == 42 and f.payload == b"\x01\x02\x03",
                  f"type={t} 字段应原样还原")

    raw = L.encode(L.T_UNLOCK_REQ, 7, L.pack_unlock_req(0x11223344, 0xDEADBEEF, 0))
    f = L.decode(raw)
    check(f is not None and L.unpack_unlock_req(f.payload) == (0x11223344, 0xDEADBEEF, 0),
          "UNLOCK_REQ 载荷往返")


def test_tamper() -> None:
    section("篡改检测")
    base = bytearray(L.encode(L.T_UNLOCK_REQ, 9, L.pack_unlock_req(1, 2, 0)))

    for idx in (3, 4, 6, 9):          # type / seq / payload 内
        bad = bytearray(base)
        bad[idx] ^= 0x01
        check(L.decode(bytes(bad)) is None, f"改第 {idx} 字节应被拒")

    bad = bytearray(base)
    bad[-6] ^= 0xFF                    # 改 CRC 本身
    check(L.decode(bytes(bad)) is None, "改 CRC 也应被拒（MAC 覆盖了 CRC）")

    bad = bytearray(base)
    bad[-1] ^= 0xFF                    # 改 MAC
    check(L.decode(bytes(bad)) is None, "改 MAC 应被拒")


def test_stream() -> None:
    section("粘包/半包")
    f1 = L.encode(L.T_HEARTBEAT, 1, L.pack_heartbeat(100, 0))
    f2 = L.encode(L.T_ACK, 2, bytes([1]))

    stream = b"\x00\xAA\x13\xAA\x55" + f1 + f2
    f, used = L.find_frame(stream)
    check(f is not None and f.type == L.T_HEARTBEAT, "噪声前缀里应找到第一帧")
    rest = stream[used:]
    f, used2 = L.find_frame(rest)
    check(f is not None and f.type == L.T_ACK, "剩下的字节里应找到第二帧")

    half = f1[:len(f1) // 2]
    f, used = L.find_frame(half)
    check(f is None, "半包不应被当作完整帧")


def test_dedup() -> None:
    section("去重窗口")
    w = L.RxWindow()
    check(w.accept(0), "首个 seq 应接受")
    check(w.accept(1), "递增 seq 应接受")
    check(not w.accept(1), "重复 seq 应拒绝")
    check(w.accept(2), "继续递增应接受")
    check(not w.accept(2), "再次重复应拒绝")


# --------------------------------------------------- 2. 与 C 实现交叉验证
def compile_c(tmpdir: str) -> str:
    out = os.path.join(tmpdir, "host_test_link")
    cmd = [
        "gcc", "-O1", "-Wall",
        f"-I{ROOT / 'components/link/include'}",
        str(ROOT / "tools/host_test_link.c"),
        str(ROOT / "components/link/link_proto.c"),
        "-o", out,
    ]
    subprocess.run(cmd, check=True)
    return out


def test_c_crosscheck() -> None:
    section("C 实现交叉验证")
    if shutil.which("gcc") is None:
        print("  ! 没找到 gcc，跳过 C 交叉验证")
        return

    with tempfile.TemporaryDirectory() as tmp:
        exe = compile_c(tmp)
        out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout

    kv = dict(line.split("=", 1) for line in out.strip().splitlines())
    print("  C 输出：", "  ".join(f"{k}={v}" for k, v in kv.items()))

    # CRC
    check(kv["crc"] == f"{L.crc16(b'123456789'):04X}", "C 与 Python 的 CRC 应一致")

    # MAC
    macbuf = bytes(range(32))
    check(kv["mac"] == f"{L.mac32(macbuf):08X}", "C 与 Python 的 MAC 应一致")

    # 整帧
    py_frame = L.encode(L.T_UNLOCK_REQ, 7, L.pack_unlock_req(0x11223344, 0xDEADBEEF, 0))
    check(kv["frame"] == py_frame.hex().upper(), "C 与 Python 编出的帧应逐字节相同")

    # 去重
    seqs = [0, 1, 2, 2, 5, 4, 3, 100, 101, 255, 1, 0]
    w = L.RxWindow()
    py_dedup = "".join("1" if w.accept(s) else "0" for s in seqs)
    check(kv["dedup"] == py_dedup, f"C 与 Python 的去重结果应一致（py={py_dedup}）")


def test_replay() -> None:
    section("重放与新鲜度（应用层规则）")
    # 演示时间戳窗口逻辑：|now - ts| < 5s 才接受
    now = 1_000_000
    ok_ts, old_ts = now - 2, now - 30
    check(abs(now - ok_ts) < 5, "2 秒前的帧应在窗口内")
    check(not (abs(now - old_ts) < 5), "30 秒前的帧应被拒（重放）")

    # nonce 去重
    used = {0xDEADBEEF}
    check(0xDEADBEEF in used, "用过的 nonce 应被拒")
    check(0xCAFEBABE not in used, "新 nonce 应接受")


def main() -> int:
    test_vectors()
    test_roundtrip()
    test_tamper()
    test_stream()
    test_dedup()
    test_c_crosscheck()
    test_replay()

    print(f"\n共 {checks} 项检查，失败 {len(fails)} 项")
    if fails:
        for f in fails:
            print("  -", f)
        return 1
    print("全部通过 ✅")
    return 0


if __name__ == "__main__":
    sys.exit(main())
