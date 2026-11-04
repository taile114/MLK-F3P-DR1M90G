#!/usr/bin/env python3
"""API v1 example: python3 model_fault_example.py --grade 2 --hold 12."""
import argparse
import json
import os
import socket
import time
import uuid


def publish(state, path=None):
    """Return the server reply; raise on transport/persistence/validation error."""
    payload = json.dumps(state, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
    if len(payload) > 1000:
        raise ValueError("request exceeds 1000 bytes")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(2)
        client.connect(path or os.environ.get("MLK_FAULT_SOCKET", "/run/mlk-fault.sock"))
        client.sendall(payload)
        reply = b""
        while not reply.endswith(b"\n"):
            part = client.recv(1024)
            if not part or len(reply) + len(part) > 1024:
                raise RuntimeError("incomplete or oversized response")
            reply += part
    result = json.loads(reply)
    if not result.get("ok"):
        raise RuntimeError(result.get("error", "fault report rejected"))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--grade", type=int, choices=range(4), default=1)
    parser.add_argument("--class", dest="cls", type=int, choices=(1, 2), default=1)
    parser.add_argument("--hold", type=float, default=12)
    args = parser.parse_args()
    state = dict(api=1, valid=True, **{"class": args.cls}, grade=args.grade,
                 id=str(uuid.uuid4()), faultTimeUs=str(time.time_ns() // 1000),
                 message="model API example")
    try:
        deadline = time.monotonic() + max(1, args.hold)
        while time.monotonic() < deadline:
            print(publish(state), flush=True)
            time.sleep(1)
    finally:
        publish({"api": 1, "valid": True, "class": 0, "grade": 0})


if __name__ == "__main__":
    main()
