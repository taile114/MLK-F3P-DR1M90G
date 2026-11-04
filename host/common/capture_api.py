#!/usr/bin/env python3
"""Send one capture API v1 JSON request from stdin to the running local app."""
import argparse
import json
import socket
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--socket", required=True)
args = parser.parse_args()
request = json.load(sys.stdin)
data = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
if len(data) > 1000:
    parser.error("request exceeds 1000 bytes")
try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(10)
        client.connect(args.socket)
        client.sendall(data)
        response = bytearray()
        while b"\n" not in response:
            chunk = client.recv(65536)
            if not chunk:
                raise ValueError("incomplete API response")
            response.extend(chunk)
        reply = json.loads(response)
        print(json.dumps(reply, ensure_ascii=False, indent=2))
        sys.exit(0 if reply.get("ok") else 1)
except (OSError, ValueError) as error:
    print(str(error), file=sys.stderr)
    sys.exit(1)
