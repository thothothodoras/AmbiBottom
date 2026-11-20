#!/bin/bash
# v0.34 end to end, Mode B (stock WLED fed over DDP by the bridge), on the REAL output path:
# real DTLS stream (tools/sender) -> bridge --wled-ip -> tools/mock_wled.py, which copies
# WLED 16.0.1's current estimate and limiter (bus_manager.cpp) bit for bit.
#  A) version v0.34; default ma_per_led_full_white 45 (measured 2026-11-17)
#  B) WLED's estimate of the frames it receives = the bridge's own estimate (same model:
#     1 mA idle + (r+g+b)/765 x 45), within 2 mA
#  C) with WLED "Maximum current" = budget + 120 (Mode B rule), WLED never has to dim a
#     bridge frame: its unlimited estimate stays <= the budget, also when the bridge limits
#  D) mDNS: the address is published with -R (no reverse entry): tools/mdns_args_test.sh runs
#     the bridge in a private network namespace (its SSDP cannot reach the real TV)
#  E) tools/wled_check.py against the mock: settings report + DDP test pass
# usage: tools/v034_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains "state-dir /tmp/e2e_v34"
# or "mock_wled.py 18634" (pkill below).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
M=18637; R=18632; D=12634; W=14634; WH=18634; S=/tmp/e2e_v34
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1

pkill -9 -f "state-dir $S" 2>/dev/null; pkill -f "mock_wled.py $WH" 2>/dev/null; sleep 0.5; rm -rf "$S"
V=$("$BIN" --version 2>&1 | head -1)
check "A version line says v0.34 or later ($V)" "$(echo "$V" | grep -Eq 'v0\.(3[4-9]|[4-9][0-9])'; echo $?)"

python3 "$T/mock_wled.py" $WH $W --leds 43 --ledma 45 --maxpwr 570 > "$S.wled.log" 2>&1 &
WP=$!; sleep 0.6
AMBI_NO_DISCOVERY=1 "$BIN" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet --pairing auto \
  --wled-ip 127.0.0.1 --wled-port $W > "$S.log" 2>&1 &
BP=$!; sleep 3
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#v34","generateclientkey":true}')
U=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])')
K=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])')
curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
SENDER_LAMPS=5 "$T/sender" "$U" "$K" $D 60 >/dev/null 2>&1 &
SP=$!; sleep 1.5

G=$(curl -s http://127.0.0.1:$M/api/settings/strip | python3 -c 'import sys,json;print(json.load(sys.stdin)["defaults"]["ma_per_led_full_white"])')
check "A default mA per LED = 45 ($G)" "$([ "$G" = 45 ]; echo $?)"

# B) both estimates of the same still frame (mock keeps the last frame; the sender's lamps are fixed)
both() {
  local b w
  b=$(curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;d=json.load(sys.stdin)["output"];L=d["leds"];print(round(43+sum(sum(c) for c in L)*45/765,1), d["estimated_ma"])')
  w=$(curl -s http://127.0.0.1:$WH/json/info | python3 -c 'import sys,json;print(json.load(sys.stdin)["leds"]["pwr"]-120)')
  echo "$b $w"
}
curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d '{"current_budget_ma":450,"peak_drift_period_s":0,"rise_ms":0}' >/dev/null
sleep 1.5
B=$(both)
echo "       bridge (from its LEDs, own estimate) / WLED: $B"
check "B WLED's estimate of the received frame = the bridge's estimate within 2 mA" \
  "$(python3 -c "import sys;a,b,w=map(float,'$B'.split());sys.exit(0 if abs(a-w)<=2 and abs(b-w)<=2 else 1)"; echo $?)"

# C) WLED's own limit never needed, at the default budget and while the bridge limits hard
for bud in 450 150; do
  curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d "{\"current_budget_ma\":$bud}" >/dev/null
  sleep 1; curl -s http://127.0.0.1:$WH/mock/reset >/dev/null; sleep 3
  X=$(curl -s http://127.0.0.1:$WH/json/info | python3 -c 'import sys,json;m=json.load(sys.stdin)["mock"];print(m["max_unlimited"],m["frames"])')
  O=$(curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;d=json.load(sys.stdin)["output"];print(d["estimated_ma"], d.get("limit",{}).get("limited"))')
  echo "       budget $bud: WLED highest estimate, frames = $X; bridge estimate, limited = $O"
  check "C budget $bud (WLED max $((bud+120))): WLED's unlimited estimate <= $bud on every frame, frames arrived" \
    "$(python3 -c "import sys;m,f='$X'.split();sys.exit(0 if int(f)>40 and int(m)<=$bud+1 else 1)"; echo $?)"
done

# D) mDNS -R, by running the bridge in a private network namespace (tools/mdns_args_test.sh)
bash "$T/mdns_args_test.sh" "$BIN" > "$S.mdns.log" 2>&1
check "D avahi-publish-address is started with -R (run in a private netns, fake avahi; $S.mdns.log)" "$?"

kill $SP 2>/dev/null; kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null

# E) the device tool against the mock (strip on its own USB: max 570 = 450 + 120)
E=$(python3 "$T/wled_check.py" 127.0.0.1 --http-port $WH --port $W --ddp-test 43 2>&1)
EC=$?
echo "$E" | sed 's/^/       | /' | grep -E 'FAIL|ABL acts|estimate while'
check "E wled_check.py: settings ok, ABL-on-DDP and no-transition checks pass (exit $EC)" "$([ $EC = 0 ]; echo $?)"

kill $WP 2>/dev/null
echo
[ $RC = 0 ] && echo "v034_test: PASS" || echo "v034_test: FAIL"
exit $RC
