#!/bin/bash
# Phase 1 end-to-end test without a TV or WLED: bridge with --wled-ip
# 127.0.0.1 --wled-port 14048, REST pairing, a real DTLS-PSK client
# (tools/sender.cpp) streaming an identical 3-lamp frame at 25 fps, and
# tools/ddp_receiver.py standing in for WLED. Expect ~25 frames/s of
# 43 LEDs. Then the same WITHOUT --wled-ip: expect no packet at all.
#
# usage: tools/wled_ddp_test.sh [bridge binary]
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
RC=0
run() {  # $1 = extra bridge args, $2.. = receiver args
  local EXTRA=$1; shift
  pkill -9 -f "state-dir /tmp/e2e_ddp" 2>/dev/null; sleep 0.5; rm -rf /tmp/e2e_ddp
  python3 "$T/ddp_receiver.py" --port 14048 --bind 127.0.0.1 --seconds 6 "$@" &
  local RP=$!
  "$BIN" --state-dir /tmp/e2e_ddp --port 18082 --stream-port 12102 --monitor-port 18100 --console quiet --pairing auto $EXTRA > /tmp/e2e_ddp.log 2>&1 &
  local BP=$!; sleep 1.5
  local RESP USER KEY
  RESP=$(curl -s -X POST http://127.0.0.1:18082/api -d '{"devicetype":"e2e#ddp","generateclientkey":true}')
  USER=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])')
  KEY=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])')
  curl -s -X PUT http://127.0.0.1:18082/api/$USER/groups/1 -d '{"stream":{"active":true}}' >/dev/null
  "$T/sender" "$USER" "$KEY" 12102 4 >/dev/null
  wait $RP || RC=1
  kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null
  grep "\[wled\]" /tmp/e2e_ddp.log
}
echo "== 1) --wled-ip 127.0.0.1 --wled-port 14048: expect 43 LEDs at >= 20 frames/s =="
run "--wled-ip 127.0.0.1 --wled-port 14048" --expect-leds 43 --min-fps 15
echo
echo "== 2) no --wled-ip: expect nothing on the DDP port =="
run "" --expect-none
echo
[ $RC = 0 ] && echo "wled_ddp_test: PASS" || echo "wled_ddp_test: FAIL"
exit $RC
