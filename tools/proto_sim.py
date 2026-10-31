#!/usr/bin/env python3
"""双板协议模拟器 —— 用 PC + USB-TTL 扮演其中一块板，不需要两块板都在手边。

    # 假装自己是 GD32：收 ESP32 发来的帧、打印解析结果、回 ACK + 心跳
    python3 tools/proto_sim.py --port /dev/ttyUSB0 --role gd32

    # 假装自己是 ESP32：主动发开锁请求，看 GD32 怎么回
    python3 tools/proto_sim.py --port /dev/ttyUSB0 --role esp32 --send-unlock 3

    # 测协议负样本
    ... --role esp32 --tamper      # 发一帧被改过 1 字节的请求（应被拒）
    ... --role esp32 --replay      # 同一帧连发两次（第二次应判重复）

依赖：pyserial（pip install pyserial）
"""

from __future__ import annotations

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link_proto as L  # noqa: E402

try:
    import serial
except ImportError:  # pragma: no cover
    print("缺少 pyserial：pip install pyserial", file=sys.stderr)
    raise SystemExit(2)


class Sim:
    def __init__(self, args):
        self.args = args
        self.seq = 0
        self.rx_buf = b""
        self.rx_win = L.RxWindow()
        self.n_ok = 0
        self.n_bad = 0
        self.n_dup = 0
        self.last_raw = None

    # ---------------------------------------------------------- 发送
    def next_seq(self) -> int:
        s = self.seq
        self.seq = (self.seq + 1) & 0xFF
        return s

    def send(self, port, msg_type, payload=b"", seq=None):
        s = self.next_seq() if seq is None else seq
        raw = L.encode(msg_type, s, payload)
        port.write(raw)
        port.flush()
        print(f"  → TX {L.TYPE_NAME.get(msg_type, msg_type):<10} seq={s:<3} {raw.hex().upper()}")
        return raw

    # ---------------------------------------------------------- 接收
    def poll(self, port, handle=True):
        chunk = port.read(256)
        if chunk:
            self.rx_buf += chunk
        while True:
            f, used = L.find_frame(self.rx_buf)
            self.rx_buf = self.rx_buf[used:]
            if f is None:
                break
            self.on_frame(port, f) if handle else None

    def on_frame(self, port, f: L.Frame):
        self.n_ok += 1
        desc = ""
        try:
            if f.type == L.T_UNLOCK_REQ:
                ts, nonce, reason = L.unpack_unlock_req(f.payload)
                desc = f"ts={ts} nonce=0x{nonce:08X} reason={reason}"
            elif f.type == L.T_UNLOCK_ACK:
                res, remain = L.unpack_unlock_ack(f.payload)
                desc = f"result={L.RESULT_NAME.get(res, res)} remain={remain}"
            elif f.type == L.T_HEARTBEAT:
                epoch, status = L.unpack_heartbeat(f.payload)
                desc = f"epoch={epoch} status=0x{status:02X}"
        except ValueError as e:
            desc = f"载荷解析失败({e})"

        dup = "" if self.rx_win.accept(f.seq) else "  [重复帧]"
        if dup:
            self.n_dup += 1
        print(f"  ← RX {f.name:<10} seq={f.seq:<3} len={len(f.payload):<2} {desc}{dup}")
        return desc

    # ---------------------------------------------------------- 角色
    def role_gd32(self, port):
        print("角色：GD32（收帧 → 回 ACK，每秒发一次心跳）")
        t_hb = 0.0
        t0 = time.time()
        while True:
            self.poll(port)
            if time.time() - t_hb >= 1.0:
                t_hb = time.time()
                epoch = int(time.time()) & 0xFFFFFFFF
                self.send(port, L.T_HEARTBEAT, L.pack_heartbeat(epoch, 0x01))
            time.sleep(0.01)
            if self.args.duration and time.time() - t0 > self.args.duration:
                break

    def role_esp32(self, port):
        print("角色：ESP32（主动发开锁请求，等待 GD32 的 ACK / UNLOCK_ACK）")
        n = self.args.send_unlock
        sent = 0
        t0 = time.time()
        next_send = 0.0

        while True:
            self.poll(port)
            now = time.time()

            if sent < n and now >= next_send:
                ts = int(now) & 0xFFFFFFFF
                nonce = (int(now * 1000) & 0xFFFFFFFF) | 0x80000000
                raw = self.send(port, L.T_UNLOCK_REQ,
                                L.pack_unlock_req(ts, nonce, 0))
                self.last_raw = raw
                sent += 1
                next_send = now + max(1.0, self.args.interval)

                if self.args.tamper:
                    bad = bytearray(raw)
                    bad[9] ^= 0x01              # 改 payload 里的时间戳
                    port.write(bad)
                    port.flush()
                    print(f"  → TX [篡改帧] {bytes(bad).hex().upper()}（期望被拒）")

                if self.args.replay:
                    port.write(raw)             # 原样重发，期望判重复
                    port.flush()
                    print("  → TX [重放帧] 同一 seq 再发一次（期望判重复）")

            time.sleep(0.01)
            if sent >= n and self.args.duration and now - t0 > self.args.duration:
                break
            if sent >= n and not self.args.duration:
                time.sleep(1.0)
                break

    def run(self):
        # serial_for_url 同时支持真实设备路径和 loop:// 自环（方便无硬件自测）
        port = serial.serial_for_url(self.args.port, self.args.baud, timeout=0.05)
        print(f"串口 {self.args.port} @ {self.args.baud}")
        try:
            if self.args.role == "gd32":
                self.role_gd32(port)
            else:
                self.role_esp32(port)
        except KeyboardInterrupt:
            print("\n中断")
        finally:
            port.close()
            print(f"\n统计：好帧 {self.n_ok}  重复 {self.n_dup}  坏帧 {self.n_bad}")


def main() -> int:
    ap = argparse.ArgumentParser(description="双板协议模拟器")
    ap.add_argument("--port", required=True, help="串口，如 /dev/ttyUSB1")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--role", choices=["esp32", "gd32"], default="gd32")
    ap.add_argument("--send-unlock", type=int, default=1, help="esp32 角色发几次开锁请求")
    ap.add_argument("--interval", type=float, default=2.0, help="两次请求间隔（秒）")
    ap.add_argument("--duration", type=float, default=0, help="运行多少秒后退出，0=一直跑")
    ap.add_argument("--tamper", action="store_true", help="附带一帧被篡改的请求")
    ap.add_argument("--replay", action="store_true", help="附带一次原样重放")
    args = ap.parse_args()

    Sim(args).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
