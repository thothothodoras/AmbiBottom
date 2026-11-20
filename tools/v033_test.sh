#!/bin/bash
# v0.33 end to end, without a TV or WLED (a UDP listener stands in for WLED's DDP port):
#  A) soft start: the first frames of a stream ramp up over rise_ms (not one step to the budget)
#  B) Exit (SIGINT) while streaming: the strip gets black frames, then nothing
#  C) Stop from the page while streaming: black frames
#  D) Restart from the page while streaming: NO black frame, the stream goes on
#  E) the budget lowered while the last frame is held: the held frame is cut to the new budget
#  F) the build-time ceiling: a budget above it is refused by the form and clamped by the API
#  G) ma_scale_r/g/b reach the estimate; corner_js_curve / wear_strength are accepted settings
# Run with AMBI_NO_DISCOVERY=1 while a real bridge is up.
# usage: tools/v033_test.sh [bridge binary]
# Same pkill caution as the other tests: never run a command line containing "state-dir /tmp/e2e_v33".
T=$(cd "$(dirname "$0")" && pwd)
SRC_BIN=${1:-$T/../build/ambi_bridge}
S=/tmp/e2e_v33
B=/tmp/e2e_v33bin
R=18433; D=12433; M=18439; W=14433
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
post_local() { curl -s -X POST "http://127.0.0.1:$M$1" -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' -d "$2"; }
code_local() { curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$M$1" -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' -d "$2"; }

pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S" "$S".* "$B"; mkdir -p "$B"
cp "$SRC_BIN" "$B/ambi_bridge"
start() {
  "$B/ambi_bridge" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet --pairing auto \
    --wled-ip 127.0.0.1 --wled-port $W >> "$S.log" 2>&1 &
  BP=$!; sleep 1.5
}
pair() {
  RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#v33","generateclientkey":true}')
  U=$(echo "$RESP" | json 'd[0]["success"]["username"]')
  K=$(echo "$RESP" | json 'd[0]["success"]["clientkey"]')
  curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null
  curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
}
listen() { python3 "$T/ddp_listener.py" --port $W --seconds "$1" --out "$2" > /dev/null & LP=$!; sleep 0.3; }
# frames of a recording as [t, estimated mA (idle 1 + 60 per LED at white), LED count]; $2 = python expression over F
frames() {
  python3 - "$1" "$2" <<'EOF'
import json, sys
F = []
for p in json.load(open(sys.argv[1])):
    b = bytes.fromhex(p["hex"])[10:]
    n = len(b) // 3
    F.append([p["t"], n * 1.0 + sum(b) / 765.0 * 60.0, n, sum(b)])
black = [f for f in F if f[3] == 0]
lit = [f for f in F if f[3] > 0]
print(eval(sys.argv[2]))
EOF
}

start; pair
# every lamp far over the budget, so the limiter is at work: 200 mA per LED, budget 450
post_local /api/settings/strip '{"ma_per_led_full_white":200,"current_budget_ma":450,"rise_ms":120}' > /dev/null

echo "== A) soft start + B) exit =="
listen 7 "$S.a.json"
"$T/sender" "$U" "$K" $D 4 > /dev/null & SP=$!
sleep 2.2
kill -INT $BP; wait $BP 2>/dev/null; wait $SP 2>/dev/null; wait $LP
# the listener's own estimate uses 60 mA per LED; the bridge works with 200: compare shapes, not mA
V=$(frames "$S.a.json" 'round(lit[0][1]/max(f[1] for f in lit),2), round(lit[1][1]/max(f[1] for f in lit),2), [round(f[1]/max(g[1] for g in lit),2) for f in lit[:5]]')
echo "       first frames as a share of the steady level: $V"
check "A first frame of the stream is well under the steady level (was 100 % until v0.32)" "$(frames "$S.a.json" '0 if 0 < lit[0][1] < 0.6*max(f[1] for f in lit) else 1')"
check "A it gets there within 5 frames and never goes above the steady level" "$(frames "$S.a.json" '0 if lit[4][1] >= 0.97*max(f[1] for f in lit) and all(a[1] <= b[1]+1e-9 for a,b in zip(lit[:5],lit[1:6])) else 1')"
check "B exit: the last frames are black (>= 3), nothing lit after the first black one" "$(frames "$S.a.json" '0 if len(black) >= 3 and black[0][0] > lit[-1][0] and len(lit) > 20 else 1')"
check "B log: exit: strip set to black" "$(grep -q 'exit: strip set to black' "$S.log"; echo $?)"

echo "== C) Stop from the page =="
start; pair
listen 7 "$S.c.json"
"$T/sender" "$U" "$K" $D 4 > /dev/null & SP=$!
sleep 2.2
C=$(post_local /api/service '{"action":"stop"}' | json 'd.get("ok")')
wait $SP 2>/dev/null; wait $LP
check "C stop accepted ($C); black frames (>= 3) end the recording, nothing lit after" "$([ "$C" = True ] && frames "$S.c.json" '0 if len(black) >= 3 and black[0][0] > lit[-1][0] and len(lit) > 20 else 1')"
check "C log: stop: strip set to black" "$(grep -q 'stop: strip set to black' "$S.log"; echo $?)"
pkill -INT -f "state-dir $S" 2>/dev/null; sleep 1.5; pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.3

echo "== D) Restart from the page =="
start; pair
N0=$(grep -c 'strip set to black' "$S.log")
listen 9 "$S.d.json"
"$T/sender" "$U" "$K" $D 7 > /dev/null & SP=$!
sleep 2.2
C=$(post_local /api/restart '' | json 'd.get("ok")')
wait $SP 2>/dev/null; wait $LP
N1=$(grep -c 'strip set to black' "$S.log")
V=$(frames "$S.d.json" 'len(black), len([f for f in lit if f[0] > 5.0])')
check "D restart accepted ($C): no black frame at the restart, frames flow again after it (black, lit after 5 s: $V; log lines $N0 -> $N1)" \
  "$([ "$C" = True ] && [ "$N0" = "$N1" ] && frames "$S.d.json" '0 if len([f for f in lit if f[0] > 5.0]) > 10 and not [f for f in black if f[0] < 7.0] else 1')"
pkill -INT -f "state-dir $S" 2>/dev/null; sleep 1.5; pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.3

echo "== E) budget lowered while the last frame is held =="
start; pair
post_local /api/settings/strip '{"ma_per_led_full_white":60,"current_budget_ma":800,"rise_ms":0,"loss_hold_ms":9000}' > /dev/null
listen 6 "$S.e.json"
"$T/sender" "$U" "$K" $D 2 > /dev/null & SP=$!
sleep 1.5
LIVE=$(curl -s http://127.0.0.1:$M/api/state | json 'round(d["output"]["estimated_ma"])')
wait $SP 2>/dev/null
sleep 0.5
post_local /api/settings/strip '{"current_budget_ma":200}' > /dev/null
sleep 1.5
# (the page shows no strip values while the stream is stopped - as before v0.33 - so the mA come from the wire)
HELD=$(curl -s http://127.0.0.1:$M/api/state | json 'round(d["output"]["limit"]["target_ma"]), d["output"]["limit"]["output_phase"]')
wait $LP
V=$(frames "$S.e.json" 'round(max(f[1] for f in F)), round(F[-1][1]), len(F)')
echo "       live $LIVE mA; held after the change: $HELD; recorder (max, last, frames): $V"
# (hold lasts 9 s here, so the last recorded frame is the held one, not a fading one)
check "E live above 200 mA; the held frame is cut to just under 200 mA (not black, not the old 800) on the wire; page phase = hold" \
  "$([ "$LIVE" -gt 200 ] && [ "${HELD#* }" = hold ] && frames "$S.e.json" '0 if max(f[1] for f in F) > 700 and 170 <= F[-1][1] <= 200.0 else 1')"

echo "== F) the ceiling =="
MAXB=$(curl -s http://127.0.0.1:$M/api/settings/strip | json 'd["limits"]["current_budget_ma_max"]')
check "F /api/settings/strip says the ceiling ($MAXB mA)" "$([ "$MAXB" = 900 ] || [ "$MAXB" = 900.0 ]; echo $?)"
C=$(code_local /api/settings/strip/validated "{\"current_budget_ma\":$((${MAXB%.*} + 1))}")
check "F the form's endpoint refuses ceiling + 1 (HTTP $C)" "$([ "$C" = 422 ]; echo $?)"
V=$(post_local /api/settings/strip '{"current_budget_ma":5000}' | json 'd["settings"]["current_budget_ma"]')
check "F the lenient endpoint clamps 5000 to the ceiling ($V)" "$([ "${V%.*}" = "${MAXB%.*}" ]; echo $?)"
V=$(curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d '{"current_budget_ma":800}' | json 'd["current_budget_ma"]')
check "F 800 stays 800 ($V); settings.json has it" "$([ "${V%.*}" = 800 ] && [ "$(json 'd["current_budget_ma"]' < $S/settings.json)" = 800 ]; echo $?)"

echo "== G) new settings: defaults, channel scales, wear from a seeded log, pages =="
G=$(curl -s http://127.0.0.1:$M/api/settings/strip)
V=$(echo "$G" | json '[d["defaults"][k] for k in ("ma_scale_r","ma_scale_g","ma_scale_b","wear_strength","corner_js_curve","corner_js_full_scale")]')
check "G defaults 1 / 1 / 1, wear 0, linear, 23 ($V)" "$([ "$V" = "[1, 1, 1, 0, 'linear', 23]" ]; echo $?)"
C=$(code_local /api/settings/strip/validated '{"ma_scale_r":5}'); C2=$(code_local /api/settings/strip/validated '{"corner_js_curve":"gamma"}'); C3=$(code_local /api/settings/strip/validated '{"wear_strength":1.5}')
check "G the form's endpoint refuses ma_scale_r 5, curve 'gamma', wear 1.5 ($C $C2 $C3)" "$([ "$C $C2 $C3" = "422 422 422" ]; echo $?)"
kill -INT $BP 2>/dev/null; sleep 1.5; pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.3
# a log in which LEDs 8..14 have worked 50 % more than the rest (this test's own state dir)
python3 -c '
import json, sys
mah = [60.0] * 43
for i in range(8, 15): mah[i] = 90.0
t = {"since": "2026-01-01 00:00:00", "stream_s": 36000, "over_s": 0, "frames": 900000, "frames_limited": 0, "max_wanted_ma": 0,
     "mah": mah, "s_hot": [0] * 43, "s_peak": [0] * 43, "avg_level": [0.2] * 43}
json.dump({"version": 1, "all_time": t, "since_reset": dict(t), "days": [], "episodes": []}, open(sys.argv[1], "w"))
' "$S/strip_stats.json"
start; pair
post_local /api/settings/strip '{"ma_per_led_full_white":200,"current_budget_ma":450,"rise_ms":0,"peak_emphasis":0.5,"peak_drift_leds":0,"wear_strength":0,"ma_scale_r":1,"ma_scale_g":1,"ma_scale_b":1,"loss_hold_ms":1000}' > /dev/null
WV=$(curl -s http://127.0.0.1:$M/api/strip_stats | json 'len(d["wear"]), max(d["wear"]), d["wear"][11], d["wear"][30]')
check "G /api/strip_stats has the wear the limiter sees: 43 LEDs, LED 11 = 1, LED 30 = 0 ($WV)" "$([ "$WV" = "43 1 1 0" ]; echo $?)"
snap() { curl -s http://127.0.0.1:$M/api/state | json 'json.dumps([sum(d["output"]["leds"][11]), sum(d["output"]["leds"][30]), 1 if d["output"]["limit"]["wear_applied"] else 0, round(d["output"]["estimated_ma"],1)])'; }
"$T/sender" "$U" "$K" $D 6 > /dev/null & SP=$!
sleep 1.2
A=$(snap)
post_local /api/settings/strip '{"wear_strength":0.8}' > /dev/null; sleep 1.0
Bv=$(snap)
post_local /api/settings/strip '{"wear_strength":0,"ma_scale_r":2.5,"ma_scale_g":2.5,"ma_scale_b":2.5}' > /dev/null; sleep 1.0
Cv=$(snap)
wait $SP 2>/dev/null
echo "       [LED 11, LED 30, wear_applied, mA]  wear 0: $A   wear 0.8: $Bv   scales 2.5: $Cv"
check "G wear 0.8: the worn LED 11 is dimmer, LED 30 not dimmer, wear_applied, still <= 450 mA" \
  "$(python3 -c "a=$A;b=$Bv;import sys;sys.exit(0 if a[2]==0 and b[2]==1 and b[0] < a[0] and b[1] >= a[1] and a[3] <= 450 and 400 < b[3] <= 450 else 1)" 2>/dev/null; echo $?)"
check "G channel scales 2.5: both LEDs are dimmer than at 1 / 1 / 1, the estimate is still <= 450 mA" \
  "$(python3 -c "a=$A;c=$Cv;import sys;sys.exit(0 if c[0] < a[0] and c[1] < a[1] and 400 < c[3] <= 450 else 1)" 2>/dev/null; echo $?)"
if command -v node > /dev/null; then
  for pg in / /ambi/settings; do
    curl -s "http://127.0.0.1:$M$pg" > "$S.page.html"
    python3 -c '
import re, sys
h = open(sys.argv[1], encoding="utf-8").read()
for i, m in enumerate(re.finditer(r"<script[^>]*>(.*?)</script>", h, re.S)):
    open("%s.page%d.js" % (sys.argv[2], i), "w", encoding="utf-8").write(m.group(1))
' "$S.page.html" "$S"
    OK=0; N=0; for f in "$S".page*.js; do N=$((N+1)); node --check "$f" 2>> "$S.node.log" || OK=1; done; rm -f "$S".page*.js
    [ "$N" -ge 1 ] || OK=1
    check "G page $pg: its $N script(s) parse (node --check)" "$OK"
  done
  check "G /ambi/settings has the new fields" "$(python3 -c '
import sys
h = open(sys.argv[1], encoding="utf-8").read()
names = ("ma_scale_r", "ma_scale_g", "ma_scale_b", "wear_strength", "corner_js_curve", "corner_js_full_scale")
sys.exit(0 if all(("id=\"%s\"" % k) in h for k in names) else 1)
' "$S.page.html"; echo $?)"
else
  echo "[skip] node is not installed: the pages' scripts were not parsed"
fi

kill -INT $BP 2>/dev/null; sleep 1.5; pkill -9 -f "state-dir $S" 2>/dev/null
[ $RC = 0 ] && echo "v033_test: PASS" || echo "v033_test: FAIL"
exit $RC
