#!/bin/bash
# JointSpace client load test against a mock TV (HTTPS + Digest, like the real
# 65OLED707) on 127.0.0.1:1926. Reports how many TLS connections / requests
# the "TV" had to serve in 10 s. v6 = ~30 connections; v7+ = 1.
#
# usage: tools/js_load_test.sh [bridge binary] [label] [mock args...]
#   mock args: --chunked | --nonce-lifetime N
#   TVPASS=wrong tools/js_load_test.sh   -> wrong-password behaviour
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}; LABEL=${2:-run}; shift 2 2>/dev/null
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
pkill -f "mock_tv.py 1926" 2>/dev/null
pkill -9 -f "state-dir /tmp/js_st" 2>/dev/null; sleep 0.5; rm -rf /tmp/js_st
python3 "$T/mock_tv.py" 1926 "$@" > /tmp/mock_$LABEL.log 2>&1 &
MP=$!; sleep 1
"$BIN" --state-dir /tmp/js_st --port 18082 --stream-port 12102 --monitor-port 18100 --tv-ip 127.0.0.1 --tv-user tvuser --tv-pass ${TVPASS:-tvpass} --console quiet > /tmp/jsb_$LABEL.log 2>&1 &
BP=$!
sleep 10
echo "== $LABEL: after 10 s =="
curl -s http://127.0.0.1:18100/api/state | python3 "$T/show_js.py" || echo "monitor: no answer"
echo "mock TV served: $(tail -1 /tmp/mock_$LABEL.log)"
kill -INT $BP; sleep 1.5; kill -9 $BP 2>/dev/null; kill $MP 2>/dev/null
exit 0
