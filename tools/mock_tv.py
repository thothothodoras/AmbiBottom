#!/usr/bin/env python3
"""Mock Philips JointSpace HTTPS server: Digest auth (qop=auth), keep-alive,
/6/ambilight/processed with 3/8/3 zones. Counts TLS connections and requests
so we can see how hard a client hits the TV.

v0.45: also the JointSpace pairing: POST /6/pair/request (the "TV" shows PIN 4321) and
POST /6/pair/grant (Digest with device id / auth_key, HMAC-SHA1 signature, PIN); the paired
device id / auth_key are then accepted by /6/ambilight/processed too.

usage: mock_tv.py PORT [--chunked] [--nonce-lifetime N]
  --chunked          send the 200 body with Transfer-Encoding: chunked
  --nonce-lifetime N rotate the nonce every N authenticated requests
                     (the next request then gets 401 stale=true)
"""
import base64, hashlib, hmac, json, os, socket, ssl, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
CHUNKED = "--chunked" in sys.argv
LIFETIME = int(sys.argv[sys.argv.index("--nonce-lifetime") + 1]) if "--nonce-lifetime" in sys.argv else 0
USER, PASS, REALM = "tvuser", "tvpass", "XTV"
HERE = os.path.dirname(os.path.abspath(__file__))
STATS = {"connections": 0, "requests": 0, "ok": 0, "challenges": 0, "bad_auth": 0}
LOCK = threading.Lock()
NONCE = {"value": os.urandom(8).hex(), "uses": 0}

def md5(s): return hashlib.md5(s.encode()).hexdigest()

PAIR_KEY = base64.b64decode("ZmVay1EQVFOaZhwQ4Kv81ypLAZNczV9sG4KkseXWn1NEk6cXmPKO/MCa9sryslvLCFMnNe4Z4CPXzToowvhHvA==")
PIN = "4321"
PENDING = {}   # device id -> (auth_key, timestamp)
PAIRED = {}    # device id -> auth_key
STATS.update({"pair_requests": 0, "pair_grants_ok": 0, "pair_grants_bad": 0, "keys": [], "power": "On"})
KEYS_OK = {"Standby", "CursorUp", "CursorDown", "CursorLeft", "CursorRight", "Confirm", "Back", "Exit", "Home",
           "Options", "Adjust", "Info", "VolumeUp", "VolumeDown", "Mute", "ChannelStepUp", "ChannelStepDown",
           "PlayPause", "Source", "AmbilightOnOff"}

def authed(self, method):
    """v0.46: Digest with tvuser/tvpass or a paired device; False = a 401 was sent."""
    a = self.headers.get("Authorization", "")
    if not a.startswith("Digest "):
        self.challenge(); return False
    p = digest_params(a)
    with LOCK:
        pw = PASS if p.get("username") == USER else PAIRED.get(p.get("username"))
    ha1 = md5(f"{p.get('username')}:{REALM}:{pw}"); ha2 = md5(f"{method}:{p.get('uri')}")
    exp = md5(f"{ha1}:{p.get('nonce')}:{p.get('nc')}:{p.get('cnonce')}:auth:{ha2}")
    if pw is None or p.get("nonce") != NONCE["value"] or p.get("response") != exp:
        with LOCK: STATS["bad_auth"] += 1
        self.challenge(); return False
    return True

def digest_params(a):
    p = {}
    for part in a[7:].split(","):
        k, _, v = part.strip().partition("=")
        p[k] = v.strip('"')
    return p

def body():
    px = lambda n, base: {str(i): {"r": (base + i * 20) % 256, "g": 100, "b": 50} for i in range(n)}
    return json.dumps({"layer1": {"left": px(3, 10), "top": px(8, 80), "right": px(3, 200)}}).encode()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def setup(self):
        super().setup()
        with LOCK: STATS["connections"] += 1
    def challenge(self, stale=False):
        with LOCK: STATS["challenges"] += 1
        msg = b"Unauthorized"
        self.send_response(401)
        hdr = f'Digest realm="{REALM}", nonce="{NONCE["value"]}", qop="auth", opaque="abc123"'
        if stale: hdr += ', stale=true'
        self.send_header("WWW-Authenticate", hdr)
        self.send_header("Content-Length", str(len(msg)))
        self.end_headers(); self.wfile.write(msg)
    def do_GET(self):
        with LOCK: STATS["requests"] += 1
        if self.path == "/6/powerstate":  # v0.46
            if authed(self, "GET"):
                with LOCK: st = STATS["power"]
                _json(self, 200, {"powerstate": st})
            return
        if self.path != "/6/ambilight/processed":
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        a = self.headers.get("Authorization", "")
        if not a.startswith("Digest "):
            return self.challenge()
        p = {}
        for part in a[7:].split(","):
            k, _, v = part.strip().partition("=")
            p[k] = v.strip('"')
        with LOCK:
            nonce_ok = p.get("nonce") == NONCE["value"]
        if not nonce_ok:
            return self.challenge(stale=True)
        with LOCK:
            pw = PASS if p.get("username") == USER else PAIRED.get(p.get("username"))
        ha1 = md5(f"{p.get('username')}:{REALM}:{pw}"); ha2 = md5(f"GET:{p.get('uri')}")
        exp = md5(f"{ha1}:{p['nonce']}:{p.get('nc')}:{p.get('cnonce')}:auth:{ha2}")
        if pw is None or p.get("response") != exp:
            with LOCK: STATS["bad_auth"] += 1
            return self.challenge()
        with LOCK:
            STATS["ok"] += 1
            NONCE["uses"] += 1
            if LIFETIME and NONCE["uses"] >= LIFETIME:
                NONCE["value"] = os.urandom(8).hex(); NONCE["uses"] = 0
        b = body()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        if CHUNKED:
            self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for i in range(0, len(b), 100):
                c = b[i:i + 100]; self.wfile.write(f"{len(c):x}\r\n".encode() + c + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

def _json(self, code, obj):
    b = json.dumps(obj).encode()
    self.send_response(code)
    self.send_header("Content-Type", "application/json")
    self.send_header("Content-Length", str(len(b)))
    self.end_headers(); self.wfile.write(b)

def do_POST(self):
    n = int(self.headers.get("Content-Length", "0") or 0)
    raw = self.rfile.read(n) if n else b""
    try:
        body = json.loads(raw or b"{}")
    except ValueError:
        return _json(self, 400, {"error_id": "INVALID_JSON"})
    if self.path == "/6/input/key":  # v0.46
        if not authed(self, "POST"): return
        k = body.get("key", "")
        if k not in KEYS_OK:
            self.send_response(400); self.send_header("Content-Length", "0"); self.end_headers(); return
        with LOCK:
            STATS["keys"].append(k)
            if k == "Standby": STATS["power"] = "Standby" if STATS["power"] == "On" else "On"
        self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers(); return
    if self.path == "/6/powerstate":
        if not authed(self, "POST"): return
        with LOCK: STATS["power"] = body.get("powerstate", STATS["power"])
        self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers(); return
    if self.path == "/6/pair/request":
        dev = body.get("device", {}).get("id", "")
        if len(dev) != 32 or body.get("access", {}).get("scope") != ["read", "write", "control"]:
            return _json(self, 200, {"error_id": "INVALID_PARAMETER"})
        key = os.urandom(16).hex(); ts = int(time.time())
        with LOCK:
            PENDING[dev] = (key, ts); STATS["pair_requests"] += 1
        print(json.dumps({"tv_shows_pin": PIN}), flush=True)
        return _json(self, 200, {"error_id": "SUCCESS", "error_text": "Authorization required", "auth_key": key, "timestamp": ts, "timeout": 60})
    if self.path == "/6/pair/grant":
        dev = body.get("device", {}).get("id", "")
        with LOCK:
            pend = PENDING.get(dev)
        a = self.headers.get("Authorization", "")
        if not pend:
            return _json(self, 200, {"error_id": "INVALID_DEVICE"})
        if not a.startswith("Digest "):
            return self.challenge()
        p = digest_params(a)
        ha1 = md5(f"{dev}:{REALM}:{pend[0]}"); ha2 = md5(f"POST:{p.get('uri')}")
        exp = md5(f"{ha1}:{p.get('nonce')}:{p.get('nc')}:{p.get('cnonce')}:auth:{ha2}")
        if p.get("username") != dev or p.get("response") != exp or p.get("nonce") != NONCE["value"]:
            return self.challenge()
        auth = body.get("auth", {})
        sig = base64.b64encode(hmac.new(PAIR_KEY, (str(auth.get("auth_timestamp")) + str(auth.get("pin"))).encode(), hashlib.sha1).digest()).decode()
        if auth.get("pin") != PIN or auth.get("auth_signature") != sig or auth.get("auth_timestamp") != pend[1]:
            with LOCK: STATS["pair_grants_bad"] += 1
            return _json(self, 200, {"error_id": "INVALID_PIN"})
        with LOCK:
            PAIRED[dev] = pend[0]; PENDING.pop(dev, None); STATS["pair_grants_ok"] += 1
        return _json(self, 200, {"error_id": "SUCCESS"})
    self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()

H.do_POST = do_POST

srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
srv.daemon_threads = True
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(os.path.join(HERE, "cert.pem"), os.path.join(HERE, "key.pem"))
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
def report():
    while True:
        time.sleep(1)
        with LOCK: print(json.dumps(STATS), flush=True)
threading.Thread(target=report, daemon=True).start()
srv.serve_forever()
