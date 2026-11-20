#!/usr/bin/env python3
"""jointspace_pair.py (v0.46) - get JointSpace credentials from a Philips Android TV, without the board.

The TV shows a PIN on screen; you type it here; the script prints the username / password to
type into the AmbiBottom page (Bridge & settings -> TV connection (JointSpace)) or anywhere else.
Python 3 standard library only. The same protocol as the board's "Pair with the TV" button
(and as ha-philipsjs, MIT, Home Assistant's Philips TV library).

usage: tools/jointspace_pair.py TV_IP [--port 1926]
The TV must be on. Nothing is stored; the credentials are printed once.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request

# The shared key every JointSpace client uses for the pairing signature (public).
PAIR_KEY = base64.b64decode("ZmVay1EQVFOaZhwQ4Kv81ypLAZNczV9sG4KkseXWn1NEk6cXmPKO/MCa9sryslvLCFMnNe4Z4CPXzToowvhHvA==")


def post(url, body, auth=None):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE  # the TV has a self-signed certificate
    handlers = [urllib.request.HTTPSHandler(context=ctx)]
    if auth:
        mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
        mgr.add_password(None, url, auth[0], auth[1])
        handlers.append(urllib.request.HTTPDigestAuthHandler(mgr))
    opener = urllib.request.build_opener(*handlers)
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"}, method="POST")
    with opener.open(req, timeout=10) as r:
        return json.loads(r.read() or b"{}")


def main():
    ap = argparse.ArgumentParser(description="Pair with a Philips TV's JointSpace API (the TV shows a PIN).")
    ap.add_argument("tv_ip")
    ap.add_argument("--port", type=int, default=1926)
    ap.add_argument("--pin", help="the PIN (for scripts / tests; normally asked for)")
    a = ap.parse_args()
    base = f"https://{a.tv_ip}:{a.port}/6"
    device = {"device_name": "AmbiBottom", "device_os": "Android", "type": "native",
              "id": os.urandom(16).hex(), "app_id": "app.ambibottom", "app_name": "AmbiBottom"}
    try:
        r = post(base + "/pair/request", {"access": {"scope": ["read", "write", "control"]}, "device": device})
    except (OSError, urllib.error.URLError) as e:
        sys.exit(f"the TV did not answer on {a.tv_ip}:{a.port} ({e}). Is it on? Right IP?")
    if r.get("error_id") != "SUCCESS":
        sys.exit(f"the TV refused the pairing request: {r.get('error_id')} {r.get('error_text', '')}")
    key, ts = r["auth_key"], r["timestamp"]
    print(f"The TV shows a PIN now (for about {r.get('timeout', 60)} s).")
    pin = a.pin or input("PIN: ").strip()
    sig = base64.b64encode(hmac.new(PAIR_KEY, (str(ts) + pin).encode(), hashlib.sha1).digest()).decode()
    body = {"auth": {"auth_appId": "1", "auth_timestamp": ts, "auth_signature": sig, "pin": pin}, "device": device}
    try:
        g = post(base + "/pair/grant", body, auth=(device["id"], key))
    except (OSError, urllib.error.URLError) as e:
        sys.exit(f"the grant failed: {e}")
    if g.get("error_id") != "SUCCESS":
        sys.exit(f"the TV did not accept the PIN: {g.get('error_id')} (run again for a new PIN)")
    print("\nPaired. Type these into the AmbiBottom page (TV connection (JointSpace)):")
    print(f"  username (TV_USER): {device['id']}")
    print(f"  password (TV_PASS): {key}")
    print("Keep them private: with them anyone on your network can control the TV.")


if __name__ == "__main__":
    main()
