#!/usr/bin/env python3
"""mock_wled.py (v0.34, tests only) - a tiny stand-in for a stock WLED 16.0.1:
DDP in (UDP), /json/info, /json/cfg, /json/state, /json/live out (HTTP).
The current estimate and limiter copy WLED 16.0.1 bus_manager.cpp
(BusDigital::estimateCurrent + BusManager::applyABL, global limit):
  used = colorSum * ledma / 765 + length; limit = maxpwr - 120;
  over the limit -> newBri = limit * 255 / used + 1, used = limit; pwr = used + 120.
colorSum is taken after the brightness (255 with "force max brightness").
Realtime: frames go straight into the frame buffer (no transition), 2.5 s timeout.

usage: python3 tools/mock_wled.py HTTP_PORT DDP_PORT [--leds 10] [--ledma 45] [--maxpwr 450]
                                  [--bri 128] [--no-maxbri] [--boot-on]
"""
import argparse, json, socket, struct, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("http_port", type=int)
ap.add_argument("ddp_port", type=int)
ap.add_argument("--leds", type=int, default=10)
ap.add_argument("--ledma", type=int, default=45)
ap.add_argument("--maxpwr", type=int, default=450)
ap.add_argument("--bri", type=int, default=128)
ap.add_argument("--no-maxbri", action="store_true")
ap.add_argument("--boot-on", action="store_true")
a = ap.parse_args()

lock = threading.Lock()
pixels = [(0, 0, 0)] * a.leds
last_rt = 0.0
pwr = 0
max_unlimited = 0   # highest estimate before WLED's own limit (tests: did WLED have to dim?)
frames = 0

def estimate():
    global pwr, max_unlimited, frames
    bri = a.bri if a.no_maxbri else 255
    csum = sum((r * bri // 255) + (g * bri // 255) + (b * bri // 255) for r, g, b in pixels)
    used = csum * a.ledma // 765 + a.leds
    limit = a.maxpwr - 120 if a.maxpwr > 120 else 1
    max_unlimited = max(max_unlimited, used); frames += 1
    if a.ledma > 0 and a.maxpwr > 0 and used > limit:
        used = limit
    pwr = used + 120 if a.ledma > 0 else 0

def ddp():
    global last_rt
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", a.ddp_port))
    while True:
        d, _ = s.recvfrom(2000)
        if len(d) < 10:
            continue
        off, ln = struct.unpack(">IH", d[4:10])
        data = d[10:10 + ln]
        with lock:
            for k in range(len(data) // 3):
                i = off // 3 + k
                if i < a.leds:
                    pixels[i] = tuple(data[3 * k:3 * k + 3])
            last_rt = time.time()
            estimate()

class H(BaseHTTPRequestHandler):
    def log_message(self, *x):
        pass
    def do_GET(self):
        global max_unlimited, frames
        with lock:
            rt = time.time() - last_rt < 2.5
            if self.path == "/json/info":
                body = {"ver": "16.0.1-mock", "arch": "esp32-s3", "freeheap": 204000,
                        "leds": {"count": a.leds, "pwr": pwr if rt else 120, "maxpwr": a.maxpwr},
                        "live": rt, "lm": "DDP" if rt else "",
                        "mock": {"max_unlimited": max_unlimited, "frames": frames, "limit": a.maxpwr - 120}}
            elif self.path == "/json/cfg":
                body = {"hw": {"led": {"maxpwr": a.maxpwr, "ins": [{"start": 0, "len": a.leds, "pin": [38],
                        "order": 0, "type": 22, "maxpwr": 0, "ledma": a.ledma}]}},
                        "if": {"live": {"maxbri": not a.no_maxbri, "no-gc": True, "mso": False, "timeout": 25}},
                        "def": {"ps": 0, "on": a.boot_on, "bri": a.bri}}
            elif self.path == "/mock/reset":
                max_unlimited = 0; frames = 0; body = {"ok": True}
            elif self.path == "/json/state":
                body = {"on": True, "bri": a.bri}
            elif self.path == "/json/live":
                body = {"leds": ["%02X%02X%02X" % p for p in pixels], "n": 1}
            else:
                self.send_response(404); self.end_headers(); return
        b = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

threading.Thread(target=ddp, daemon=True).start()
ThreadingHTTPServer(("127.0.0.1", a.http_port), H).serve_forever()
