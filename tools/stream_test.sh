#!/bin/bash
# End-to-end Hue Entertainment test without a TV: pairs over REST, sets
# stream.active, then a real DTLS-PSK client (tools/sender.cpp) streams one
# IDENTICAL 3-lamp frame at 25 fps - what the TV does on a still picture.
# The monitor must say live / ~25 fps / 3 of 3 lamps the whole time
# (v5 and older said stale / 0 fps here).
#
# usage: tools/stream_test.sh [bridge binary]
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
pkill -9 -f "state-dir /tmp/e2e_st" 2>/dev/null; sleep 0.5; rm -rf /tmp/e2e_st
"$BIN" --state-dir /tmp/e2e_st --port 18081 --stream-port 12101 --monitor-port 18099 --console quiet --pairing auto > /tmp/e2e.log 2>&1 &
BP=$!; sleep 1.5
RESP=$(curl -s -X POST http://127.0.0.1:18081/api -d '{"devicetype":"e2e#test","generateclientkey":true}')
USER=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])')
KEY=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])')
curl -s -X PUT http://127.0.0.1:18081/api/$USER/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$USER" "$KEY" 12101 6 >/dev/null &
SP=$!
sleep 2.5
echo "== /api/state every 0.5 s while identical frames stream at 25 fps =="
for i in 1 2 3 4 5 6; do
  curl -s http://127.0.0.1:18099/api/state | python3 "$T/show.py"
  sleep 0.5
done
wait $SP
echo "== SSE /events messages in 2 s (expect ~33) =="
timeout 2 curl -s -N http://127.0.0.1:18099/events | grep -c '^event: state'
kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null
exit 0
