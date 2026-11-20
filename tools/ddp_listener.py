#!/usr/bin/env python3
"""A local UDP listener that stands in for a stock WLED's DDP port and writes
every datagram it gets, byte for byte, to a JSON file - for
tools/ddp_target_test.sh, which then checks the packets.

usage: python3 tools/ddp_listener.py --port 14148 --seconds 6 --out /tmp/ddp_packets.json
"""
import argparse, json, socket, time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, required=True)
ap.add_argument("--bind", default="127.0.0.1")
ap.add_argument("--seconds", type=float, default=6.0)
ap.add_argument("--out", required=True)
a = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((a.bind, a.port))
s.settimeout(0.2)
packets = []
start = time.monotonic()
while time.monotonic() - start < a.seconds:
    try:
        data, src = s.recvfrom(65535)
    except socket.timeout:
        continue
    packets.append({"t": round(time.monotonic() - start, 4), "from": src[0], "hex": data.hex()})
json.dump(packets, open(a.out, "w"))
print(f"ddp_listener: {len(packets)} datagram(s) in {a.seconds:.0f} s on {a.bind}:{a.port}")
