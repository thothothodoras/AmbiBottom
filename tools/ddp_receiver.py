#!/usr/bin/env python3
"""Stand-in for WLED's DDP listener: receives DDP on UDP, parses each packet
the way WLED does (wled00/e131.cpp handleDDPPacket: 10-byte header, byte
offset, RGB24, PUSH = show frame) and reports what a strip would show.

usage: python3 tools/ddp_receiver.py [--port 4048] [--seconds 5]
                                     [--expect-leds N] [--min-fps F] [--expect-none]
Prints one line per second (frames/s, LEDs per frame, first/middle/last colour)
and a final summary. Exit code 0 if the --expect-* / --min-fps checks pass.
"""
import argparse, socket, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=4048)
ap.add_argument("--bind", default="0.0.0.0")
ap.add_argument("--seconds", type=float, default=5.0)
ap.add_argument("--expect-leds", type=int)
ap.add_argument("--min-fps", type=float)
ap.add_argument("--expect-none", action="store_true", help="pass only if NO packet arrives")
a = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((a.bind, a.port))
s.settimeout(0.2)

frame = {}           # led index -> (r,g,b), filled by packets until PUSH
frames = []          # (time, led_count, colours) per PUSH
bad = 0
packets = 0
seqs = []
start = time.monotonic()
last_report = start
sec_frames = 0

def hexc(c):
    return "#%02x%02x%02x" % c

while time.monotonic() - start < a.seconds:
    try:
        data, src = s.recvfrom(65535)
    except socket.timeout:
        data = None
    now = time.monotonic()
    if data is not None:
        packets += 1
        if len(data) < 10 or (data[0] & 0xC0) != 0x40:
            bad += 1
            continue
        flags, seq, dtype, dest = data[0], data[1] & 0x0F, data[2], data[3]
        off = int.from_bytes(data[4:8], "big")
        ln = int.from_bytes(data[8:10], "big")
        hdr = 14 if flags & 0x10 else 10
        if len(data) < hdr + ln or off % 3 or ln % 3:
            bad += 1
            continue
        body = data[hdr:hdr + ln]
        for i in range(ln // 3):
            frame[off // 3 + i] = tuple(body[3 * i:3 * i + 3])
        seqs.append(seq)
        if flags & 0x01:  # PUSH
            n = (max(frame) + 1) if frame else 0
            cols = [frame.get(i, (0, 0, 0)) for i in range(n)]
            frames.append((now, n, cols))
            sec_frames += 1
            frame = {}
    if now - last_report >= 1.0:
        if frames:
            _, n, cols = frames[-1]
            print("%5.1f s  %3d frames/s  %3d LEDs  first %s  mid %s  last %s"
                  % (now - start, sec_frames, n, hexc(cols[0]), hexc(cols[n // 2]), hexc(cols[-1])), flush=True)
        else:
            print("%5.1f s  no DDP frames" % (now - start), flush=True)
        sec_frames = 0
        last_report = now

dur = time.monotonic() - start
ok = True
fps = len(frames) / dur if dur > 0 else 0
leds = sorted({n for _, n, _ in frames})
print("summary: %d packets, %d frames, %.1f frames/s, LED counts %s, bad packets %d, seq values %s"
      % (packets, len(frames), fps, leds or "-", bad, sorted(set(seqs)) or "-"))
if a.expect_none:
    ok = packets == 0
elif not frames:
    ok = False
if a.expect_leds is not None and frames and leds != [a.expect_leds]:
    ok = False
if a.min_fps is not None and fps < a.min_fps:
    ok = False
if bad:
    ok = False
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
