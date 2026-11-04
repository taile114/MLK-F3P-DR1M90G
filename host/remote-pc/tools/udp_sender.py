#!/usr/bin/env python3
"""Send synthetic MLK v2 frames (4 sensor channels + diagnosis) to test the PC host."""
import argparse
import math
import socket
import struct
import time

MAGIC = 0x4B4D
VER = 2
TYPE_WAVE = 0
TYPE_DIAG = 1


def pack_wave(seq, cnt, rate, ts, ch, samples):
    head = struct.pack("<HBBIIHHQB", MAGIC, VER, TYPE_WAVE, seq, cnt, rate, len(samples), ts, ch)
    head += b"\x00\x00\x00"
    body = b"".join(struct.pack("<h", s) for s in samples)
    return head + body


def pack_diag(seq, ts, cls, grade, conf, rul, infer_us):
    head = struct.pack("<HBBIIHHQ", MAGIC, VER, TYPE_DIAG, seq, 0, 0, 0, ts)
    head += struct.pack("<BBHII", cls, grade, conf, rul, infer_us)
    return head


def main():
    ap = argparse.ArgumentParser(description="Send synthetic MLK v2 frames")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--rate", type=int, default=100, help="packets per second per channel")
    ap.add_argument("--n", type=int, default=20, help="samples per packet")
    ap.add_argument("--count", type=int, default=0, help="packets per channel, 0 = forever")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = [0, 0, 0, 0]
    cnt = [0, 0, 0, 0]
    phase = [0.0, 0.0, 0.0, 0.0]
    period = 1.0 / args.rate if args.rate > 0 else 0.0
    step = 0

    while args.count == 0 or seq[0] < args.count:
        ts = int(time.time() * 1e6)
        for ch in range(4):
            samples = []
            for _ in range(args.n):
                phase[ch] += 0.02
                p = phase[ch]
                if ch == 0:
                    v = 2500 + 150 * math.sin(p * 0.2)
                elif ch == 1:
                    v = 250 + 120 * math.sin(p * 2.0)
                elif ch == 2:
                    v = 100 + 400 * math.sin(p * 8.0)
                else:
                    v = 1500 + 40 * math.sin(p * 0.1)
                samples.append(int(v))
            sock.sendto(pack_wave(seq[ch], cnt[ch], 1000, ts, ch, samples), (args.host, args.port))
            seq[ch] += 1
            cnt[ch] += args.n

        if step % 10 == 0:
            slot = (step // 10) % 4
            cls = 0 if slot < 2 else (1 if slot == 2 else 2)
            sock.sendto(pack_diag(step, ts, cls, cls, 900, 1800, 2100), (args.host, args.port))
        step += 1

        if period:
            time.sleep(period)


if __name__ == "__main__":
    main()
