#!/bin/bash
# v0.18 end-to-end: blend_mode / corner_mode on the REAL output, with a
# real DTLS stream (tools/sender: 3 lamps) and a mock JointSpace TV
# (tools/mock_tv.py on 127.0.0.1:1926) for the side colours at the corners.
# Settings are changed live through /ambi/settings (127.0.0.1) while the
# stream runs, and output.leds in /api/state must follow within a frame.
#
# usage: tools/blend_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains
# "state-dir /tmp/e2e_blend" or "mock_tv.py 1926" (pkill below).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
M=18130; R=18099; S=/tmp/e2e_blend
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1

pkill -9 -f "state-dir $S" 2>/dev/null; pkill -f "mock_tv.py 1926" 2>/dev/null; sleep 0.5; rm -rf "$S"
python3 "$T/mock_tv.py" 1926 > "$S.mock.log" 2>&1 &
MP=$!; sleep 0.8
"$BIN" --state-dir "$S" --port $R --stream-port 12130 --monitor-port $M --console quiet --pairing auto \
  --tv-ip 127.0.0.1 --tv-user tvuser --tv-pass tvpass --js-interval 0.2 > "$S.log" 2>&1 &
BP=$!; sleep 4
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#blend","generateclientkey":true}')
U=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])')
K=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])')
curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$U" "$K" 12130 25 >/dev/null 2>&1 &
SP=$!; sleep 1.5

# set <json>; then analyse output.leds with a python expression over L (list of [r,g,b])
set_and_eval() {
  curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d "$1" >/dev/null
  sleep 0.6
  curl -s http://127.0.0.1:$M/api/state | python3 -c "
import sys,json
d=json.load(sys.stdin); L=[tuple(c) for c in d['output']['leds']]; lim=d['output']['limit']
runs=[]
for c in L:
    if runs and runs[-1][0]==c: runs[-1][1]+=1
    else: runs.append([c,1])
print($2)"
}
JS=$(curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;print(json.load(sys.stdin)["sources"]["jointspace"]["status"])')
check "mock JointSpace live (status=$JS)" "$([ "$JS" = live ]; echo $?)"

# v0.33: "no limit" = frugal LEDs under the 900 mA ceiling (was a 20000 mA budget)
base='"current_budget_ma":900,"ma_per_led_full_white":1,"idle_ma_per_led":0,"rise_ms":0,"lamp_inset":0.25,"strip_layout":"inset"'   # v0.30: these checks are the inset layout (v0.18)
V=$(set_and_eval "{$base,\"blend_mode\":\"smooth\",\"corner_mode\":\"lamp\"}" 'len(L), lim["blend_mode"], lim["corner_mode"], L[0]==L[10], len(runs)>10')
check "smooth + lamp corners: 43 LEDs, ends held (LED 0 = LED 10), gradient between ($V)" "$([ "$V" = "43 smooth lamp True True" ]; echo $?)"
LAMP0=$(set_and_eval "{$base,\"blend_mode\":\"smooth\",\"corner_mode\":\"lamp\"}" 'L[0]')
V=$(set_and_eval "{$base,\"blend_mode\":\"zones\",\"corner_mode\":\"lamp\"}" 'lim["blend_mode"], len(runs), [n for _,n in runs]')
check "zones: 3 flat runs ($V)" "$([[ "$V" == "zones 3 "* ]]; echo $?)"
V=$(set_and_eval "{$base,\"blend_mode\":\"zones_mid\",\"corner_mode\":\"lamp\"}" 'lim["blend_mode"], len(runs), [n for _,n in runs]')
check "zones + in-between: 5 flat runs ($V)" "$([[ "$V" == "zones_mid 5 "* ]]; echo $?)"
V=$(set_and_eval "{$base,\"blend_mode\":\"zones\",\"corner_mode\":\"side\"}" 'lim["corner_mode"], len(runs), runs[0][0]!=runs[1][0]')
check "zones + side corners: 5 runs (corner, R, G, B, corner) ($V)" "$([ "$V" = "side 5 True" ]; echo $?)"
SIDE0=$(set_and_eval "{$base,\"blend_mode\":\"smooth\",\"corner_mode\":\"side\"}" 'L[0]')
check "smooth + side corners: LED 0 differs from the lamp-only LED 0 ($LAMP0 -> $SIDE0)" "$([ "$LAMP0" != "$SIDE0" ]; echo $?)"
# v0.33: corner_js_curve srgb = the side colour goes through the lamps' curve (darker middles)
SIDE0S=$(set_and_eval "{$base,\"blend_mode\":\"smooth\",\"corner_mode\":\"side\",\"corner_js_curve\":\"srgb\"}" 'L[0]')
V=$(python3 -c "a=$SIDE0;b=$SIDE0S;print(a!=b and all(y<=x for x,y in zip(a,b)))")
check "corner_js_curve srgb: LED 0 no brighter in any channel, and different ($SIDE0 -> $SIDE0S)" "$([ "$V" = True ]; echo $?)"
set_and_eval '{"corner_js_curve":"linear"}' 'len(L)' > /dev/null
V=$(set_and_eval '{"current_budget_ma":800,"ma_per_led_full_white":60,"idle_ma_per_led":1,"strip_layout":"inset","blend_mode":"zones_mid","corner_mode":"side","peak_emphasis":0.5}' 'd["output"]["estimated_ma"]<=800+1e-6')
check "budget 800 still respected with zones_mid + side" "$([ "$V" = True ]; echo $?)"
kill -INT $BP; sleep 1; kill -9 $BP $SP $MP 2>/dev/null
echo
[ $RC = 0 ] && echo "blend_test: PASS" || echo "blend_test: FAIL"
exit $RC
