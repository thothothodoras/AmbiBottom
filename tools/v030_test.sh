#!/bin/bash
# v0.30 end to end on the REAL output: a real DTLS stream with 5 distinct lamps
# (tools/sender, SENDER_LAMPS=5) and a mock JointSpace TV (tools/mock_tv.py on 127.0.0.1:1926).
#  A) defaults: strip_layout tv, edge_source auto (-> lamps with 5 lamps), edge 3 LEDs at 0.5
#  B) tv layout: Λ2 / Λ3 / Λ4 at LED 0 / 21 / 42; edge colour Λ1 / Λ5 mixed into the outer LEDs
#     (strength x (E - i) / E), the LEDs inside untouched
#  C) edge_source jointspace: the outer LEDs follow the TV's side, not Λ1
#  D) strip_layout inset = the v0.29 rule (no edge colour)
#  E) over budget: the peaks are spread over the whole strip (largest gap <= spacing + 1)
#  F) limits: edge_leds 99 -> 10, edge_strength 2 -> 1, unknown layout / source ignored
# usage: tools/v030_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains "state-dir /tmp/e2e_v30"
# or "mock_tv.py 1926" (pkill below). Not at the same time as blend_test.sh (same mock TV port).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
M=18137; R=18102; D=12137; S=/tmp/e2e_v30
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1

pkill -9 -f "state-dir $S" 2>/dev/null; pkill -f "mock_tv.py 1926" 2>/dev/null; sleep 0.5; rm -rf "$S"
python3 "$T/mock_tv.py" 1926 > "$S.mock.log" 2>&1 &
MP=$!; sleep 0.8
"$BIN" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet --pairing auto --lamps 5 \
  --tv-ip 127.0.0.1 --tv-user tvuser --tv-pass tvpass --js-interval 0.2 > "$S.log" 2>&1 &
BP=$!; sleep 4
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#v30","generateclientkey":true}')
U=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])')
K=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])')
curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
SENDER_LAMPS=5 "$T/sender" "$U" "$K" $D 40 >/dev/null 2>&1 &
SP=$!; sleep 1.5

# set <json>; then a python expression over L (output.leds as tuples), lim (output.limit), d (state)
set_and_eval() {
  curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d "$1" >/dev/null
  sleep 0.6
  curl -s http://127.0.0.1:$M/api/state | python3 -c "
import sys,json
d=json.load(sys.stdin); L=[tuple(c) for c in d['output']['leds']]; lim=d['output']['limit']
near=lambda a,b,t=1: all(abs(x-y)<=t for x,y in zip(a,b))
mix=lambda a,b,w: tuple(x+(y-x)*w for x,y in zip(a,b))
print($2)"
}
G=$(curl -s http://127.0.0.1:$M/api/settings/strip)
check "A defaults: strip_layout tv, edge_source auto, edge_leds 3, edge_strength 0.5" \
  "$(echo "$G" | python3 -c 'import sys,json;d=json.load(sys.stdin)["defaults"];sys.exit(0 if (d["strip_layout"],d["edge_source"],d["edge_leds"],d["edge_strength"])==("tv","auto",3,0.5) else 1)'; echo $?)"
JS=$(curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["sources"]["jointspace"]["status"],len(d["bottom"]))')
check "mock JointSpace live, 5 Hue lamps ($JS)" "$([ "$JS" = "live 5" ]; echo $?)"

# v0.33: "no limit" = frugal LEDs under the 900 mA ceiling (was a 20000 mA budget)
base='"current_budget_ma":900,"ma_per_led_full_white":1,"idle_ma_per_led":0,"rise_ms":0,"blend_mode":"zones","strip_layout":"tv"'
# B) no edge: pure Λ2 / Λ3 / Λ4, three flat zones
NOEDGE=$(set_and_eval "{$base,\"edge_leds\":0,\"edge_source\":\"auto\"}" 'json.dumps([L[0],L[10],L[21],L[42],L[32],len(L),lim["strip_layout"],lim["edge_source"]])')
echo "       edge 0:  LED 0/10/21/42/32, n, layout, source = $NOEDGE"
check "B tv layout, edge 0: 43 LEDs, LED 0 = LED 10 (Λ2), LED 42 = LED 32 (Λ4), 3 different zones, edge source none" \
  "$(python3 -c "import json,sys;a=json.loads('$NOEDGE');sys.exit(0 if a[5]==43 and a[0]==a[1] and a[3]==a[4] and len({tuple(a[0]),tuple(a[2]),tuple(a[3])})==3 and a[6]=='tv' and a[7]=='none' else 1)"; echo $?)"
# edge 1 LED at strength 1 = the plain edge colours (Λ1 / Λ5) on LED 0 / 42
PURE=$(set_and_eval "{$base,\"edge_leds\":1,\"edge_strength\":1}" 'json.dumps([L[0],L[1],L[41],L[42]])')
check "B edge 1 LED x 1.0: LED 0 / 42 = Λ1 / Λ5 (differ from Λ2 / Λ4), LED 1 / 41 untouched" \
  "$(python3 -c "import json,sys;a=json.loads('$PURE');b=json.loads('$NOEDGE');sys.exit(0 if a[0]!=b[0] and a[3]!=b[3] and a[1]==b[0] and a[2]==b[3] else 1)"; echo $?)"
E1=$(python3 -c "import json;print(json.dumps(json.loads('$PURE')[0]))"); E5=$(python3 -c "import json;print(json.dumps(json.loads('$PURE')[3]))")
A2=$(python3 -c "import json;print(json.dumps(json.loads('$NOEDGE')[0]))"); A4=$(python3 -c "import json;print(json.dumps(json.loads('$NOEDGE')[3]))")
V=$(set_and_eval "{$base,\"edge_leds\":3,\"edge_strength\":0.5}" "all(near(L[i],mix($A2,$E1,0.5*(3-i)/3)) and near(L[42-i],mix($A4,$E5,0.5*(3-i)/3)) for i in range(3)) and L[3]==tuple($A2) and L[39]==tuple($A4) and lim['edge_source']=='lamps'")
check "B default edge (3 LEDs, 0.5): LED 0/1/2 = Λ2 + 50/33/17 % of Λ1 (and mirrored), LED 3 / 39 untouched, source auto -> lamps ($V)" "$([ "$V" = True ]; echo $?)"

# C) JointSpace as the edge source (its share fades in over ~1 s after the switch: wait for 1.0)
curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d "{$base,\"edge_leds\":3,\"edge_strength\":0.5,\"edge_source\":\"jointspace\"}" >/dev/null
for k in $(seq 30); do
  [ "$(curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;print(float(json.load(sys.stdin)["output"]["limit"]["corner_weight"]))')" = 1.0 ] && break; sleep 0.2
done
V=$(set_and_eval "{$base,\"edge_leds\":3,\"edge_strength\":0.5,\"edge_source\":\"jointspace\"}" "lim['edge_source'], L[0]!=tuple($A2) and not near(L[0],mix($A2,$E1,0.5)), L[3]==tuple($A2), float(lim['corner_weight'])")
check "C edge_source jointspace: reported, LED 0 differs from Λ2 and from the Λ1 mix, LED 3 untouched ($V)" "$([ "$V" = "jointspace True True 1.0" ]; echo $?)"

# D) inset = v0.29 (lamps at 25 / 50 / 75 % ... with 5 lamps: 10 % steps; no edge colour)
V=$(set_and_eval "{\"current_budget_ma\":900,\"ma_per_led_full_white\":1,\"idle_ma_per_led\":0,\"rise_ms\":0,\"blend_mode\":\"zones\",\"strip_layout\":\"inset\",\"corner_mode\":\"lamp\",\"end_lamps\":false,\"edge_source\":\"auto\"}" "lim['strip_layout'], lim['edge_source'], L[0]==L[5], L[0]!=tuple($A2)")
check "D strip_layout inset: no edge colour, LED 0 = the first lamp held (not Λ2) ($V)" "$([ "$V" = "inset none True True" ]; echo $?)"

# E) peaks spread over the whole strip while over budget
GAPS=""
for k in 1 2 3; do
  G1=$(set_and_eval "{\"current_budget_ma\":300,\"ma_per_led_full_white\":60,\"idle_ma_per_led\":1,\"peak_emphasis\":0.5,\"peak_points\":0,\"rise_ms\":0,\"strip_layout\":\"tv\",\"blend_mode\":\"smooth\",\"peak_drift_period_s\":5}" \
    "(lambda p: max([(p[(i+1)%len(p)]-p[i])%43 or 43 for i in range(len(p))]) if len(p)==3 else -1)(sorted(lim['peak_leds'] or []))")
  GAPS="$GAPS $G1"; sleep 0.9
done
check "E over budget: 3 peaks, largest gap <= 43/3 + 1 at 3 drift phases (gaps:$GAPS)" \
  "$(python3 -c "import sys;g=[int(x) for x in '$GAPS'.split()];sys.exit(0 if all(0<x<=43/3+1 for x in g) else 1)"; echo $?)"

# F) limits
curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d '{"strip_layout":"tv","edge_source":"lamps"}' >/dev/null
curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d '{"edge_leds":99,"edge_strength":2,"strip_layout":"sideways","edge_source":"moon"}' >/dev/null
V=$(curl -s http://127.0.0.1:$M/api/settings/strip | python3 -c 'import sys,json;d=json.load(sys.stdin)["settings"];print(d["edge_leds"],d["edge_strength"],d["strip_layout"],d["edge_source"])')
check "F limits: edge_leds 10, strength 1, unknown layout / source keep tv / lamps ($V)" "$([ "$V" = "10 1 tv lamps" ]; echo $?)"
F=$(python3 -c "import json;print(json.load(open('$S/settings.json'))['strip_layout'])" 2>/dev/null || echo missing)
check "F saved to settings.json (strip_layout $F)" "$([ "$F" = tv ]; echo $?)"

kill -INT $BP; sleep 1; kill -9 $BP $SP $MP 2>/dev/null
echo
[ $RC = 0 ] && echo "v030_test: PASS" || echo "v030_test: FAIL"
exit $RC
