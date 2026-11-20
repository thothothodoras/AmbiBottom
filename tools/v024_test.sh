#!/bin/bash
# v0.24 end to end, without a TV or WLED:
#  A) version in /api/state + page footer; default drift = auto (10.5 LEDs), triangle, 300 s
#  B) unknown user -> short config / error 1 (real binary)
#  C) stream loss: hold, fade, black for loss_black_s, then nothing (DDP recorder stands in for WLED)
#  D) end_lamps with 5 lamps: lamp 5 on the last LED, lamp 4 no longer held to the end
#  E) Restart from the page: LAN refused, broken build refused (bridge keeps running), then a real restart
#  F) Restart from the terminal: r, r too late = nothing; r r = restart (on a pty)
# Same pkill caution as the other tests: never run a command line containing "state-dir /tmp/e2e_v24".
T=$(cd "$(dirname "$0")" && pwd)
SRC_BIN=${1:-$T/../build/ambi_bridge}
S=/tmp/e2e_v24
B=/tmp/e2e_v24bin
R=18094; D=12134; M=18134; W=14049
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
VER=$("$SRC_BIN" --version | awk '{print $2}')
LAN=$(hostname -I | awk '{print $1}')
post_local() { curl -s -X POST "http://127.0.0.1:$M$1" -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' -d "$2"; }

pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S" "$S.log" "$S"2 "$B"; mkdir -p "$B"
cp "$SRC_BIN" "$B/ambi_bridge"
start() {
  "$B/ambi_bridge" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet --pairing auto \
    --wled-ip 127.0.0.1 --wled-port $W "$@" >> "$S.log" 2>&1 &
  BP=$!; sleep 1.5
}
pair() {
  RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#v24","generateclientkey":true}')
  U=$(echo "$RESP" | json 'd[0]["success"]["username"]')
  K=$(echo "$RESP" | json 'd[0]["success"]["clientkey"]')
  curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
}
start --lamps 5
pair

echo "== A) version + drift defaults =="
V=$(curl -s http://127.0.0.1:$M/api/state | json 'd["bridge_version"]')
check "/api/state bridge_version = $V" "$([ "$V" = "$VER" ]; echo $?)"
check "page has the footer version slot" "$(curl -s http://127.0.0.1:$M/ | grep -q 'id="footVersion"'; echo $?)"
G=$(curl -s http://127.0.0.1:$M/api/settings/strip)
check "defaults: peak_drift_leds auto, triangle, 300 s, end_lamps off, loss 1000/1000/30" \
  "$(echo "$G" | python3 -c 'import sys,json;d=json.load(sys.stdin)["defaults"];sys.exit(0 if d["peak_drift_leds"]=="auto" and d["peak_drift_shape"]=="triangle" and d["peak_drift_period_s"]==300 and d["end_lamps"] is False and (d["loss_hold_ms"],d["loss_fade_ms"],d["loss_black_s"])==(1000,1000,30) else 1)'; echo $?)"

echo "== B) unknown user =="
NB=00000000000000000000000000000000
check "unknown user GET /config -> short config, no whitelist" \
  "$(curl -s http://127.0.0.1:$R/api/$NB/config | python3 -c 'import sys,json;d=json.load(sys.stdin);sys.exit(0 if "bridgeid" in d and "whitelist" not in d else 1)'; echo $?)"
check "unknown user GET /lights -> error 1 unauthorized user" \
  "$(curl -s http://127.0.0.1:$R/api/$NB/lights | python3 -c 'import sys,json;d=json.load(sys.stdin);e=d[0]["error"];sys.exit(0 if e["type"]==1 and e["description"]=="unauthorized user" else 1)'; echo $?)"
check "log: unknown user line" "$(grep -q "unknown user 000000000000... from 127.0.0.1 -> error 1" "$S.log"; echo $?)"
check "known user still gets its lights" "$(curl -s http://127.0.0.1:$R/api/$U/lights | python3 -c 'import sys,json;d=json.load(sys.stdin);sys.exit(0 if len(d)==5 else 1)'; echo $?)"

echo "== C) stream loss: hold 1 s, fade 1 s, black 3 s, then nothing =="
P=$(post_local /api/settings/strip '{"loss_black_s":3}')
check "loss_black_s set to 3 from the local page" "$(echo "$P" | python3 -c 'import sys,json;sys.exit(0 if json.load(sys.stdin)["settings"]["loss_black_s"]==3 else 1)'; echo $?)"
python3 - "$W" > "$S.ddp.json" <<'EOF' &
import socket, sys, time, json
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.settimeout(0.2)
t0 = time.monotonic(); out = []
while time.monotonic() - t0 < 11.0:
    try: data, _ = s.recvfrom(4096)
    except socket.timeout: continue
    if len(data) < 10: continue
    out.append([round(time.monotonic() - t0, 3), sum(data[10:]), len(data[10:]) // 3])
print(json.dumps(out))
EOF
RP=$!
sleep 0.3
"$T/sender" "$U" "$K" $D 2 > /dev/null
PH=""
for i in $(seq 1 30); do
  PH="$PH $(curl -s http://127.0.0.1:$M/api/state | json 'd["output"]["limit"]["output_phase"]')"; sleep 0.25
done
wait $RP
echo "       phases seen: $(echo $PH | tr ' ' '\n' | uniq | tr '\n' ' ')"
check "page/state shows hold -> fade -> black -> stopped" \
  "$(echo $PH | tr ' ' '\n' | uniq | tr '\n' ' ' | grep -q 'hold fade black stopped'; echo $?)"
python3 - "$S.ddp.json" > "$S.ddp.txt" <<'EOF'
import json, sys
f = json.load(open(sys.argv[1]))
stop = None  # end of the stream: the last frame before the first gap >= 0.3 s
for a, b in zip(f, f[1:]):
    if b[0] - a[0] >= 0.3: stop = a; break
full = stop[1]
after = [x for x in f if x[0] > stop[0]]
hold = [x for x in after if x[0] - stop[0] < 0.95]
fade = [x for x in after if 0 < x[1] < full]
black = [x for x in after if x[1] == 0]
lv = [x[1] for x in fade]
print(json.dumps({"full": full, "leds": stop[2], "hold": len(hold), "hold_same": all(x[1] == full for x in hold),
  "fade": len(fade), "fade_down": all(a >= b for a, b in zip(lv, lv[1:])),
  "fade_start": round(fade[0][0] - stop[0], 2) if fade else -1, "black": len(black),
  "last_black": round(black[-1][0] - stop[0], 2) if black else -1,
  "after_black": len([x for x in f if black and x[0] > black[-1][0]]),
  "silence": round(11.0 - f[-1][0], 2)}))
EOF
DD=$(cat "$S.ddp.txt"); echo "       DDP: $DD"
q() { echo "$DD" | python3 -c "import sys,json;d=json.load(sys.stdin);sys.exit(0 if ($1) else 1)"; echo $?; }
check "hold: the last frame resent unchanged during the first second" "$(q 'd["hold"]>=1 and d["hold_same"]')"
check "fade: >= 10 frames getting darker, starting ~1 s after the stop" "$(q 'd["fade"]>=10 and d["fade_down"] and 0.85<=d["fade_start"]<=1.3')"
check "black: sent ~2/s, the last one ~5 s after the stop" "$(q 'd["black"]>=5 and 4.4<=d["last_black"]<=5.4')"
check "then nothing for the rest (WLED would go to its own preset)" "$(q 'd["after_black"]==0 and d["silence"]>=2')"
check "log: fading + stopped sending lines" \
  "$(grep -q 'fading the strip to black' "$S.log" && grep -q 'stopped sending (WLED goes back' "$S.log"; echo $?)"
"$T/sender" "$U" "$K" $D 1 > /dev/null
check "stream back: log + phase live" \
  "$(grep -q 'Hue stream back after' "$S.log" && [ "$(curl -s http://127.0.0.1:$M/api/state | json 'd["output"]["limit"]["output_phase"]')" = live ]; echo $?)"

echo "== D) end_lamps with 5 lamps (zones) =="
post_local /api/settings/strip '{"strip_layout":"inset","blend_mode":"zones","corner_mode":"lamp","end_lamps":true,"current_budget_ma":900,"ma_per_led_full_white":1,"idle_ma_per_led":0}' > /dev/null
SENDER_LAMPS=5 "$T/sender" "$U" "$K" $D 1 > /dev/null &
SP=$!; sleep 0.7
ON=$(curl -s http://127.0.0.1:$M/api/state | json 'json.dumps([d["output"]["leds"][i] for i in (0,11,32,42)]+[d["output"]["limit"]["end_lamps"]])')
wait $SP
post_local /api/settings/strip '{"end_lamps":false}' > /dev/null
SENDER_LAMPS=5 "$T/sender" "$U" "$K" $D 1 > /dev/null &
SP=$!; sleep 0.7
OFF5=$(curl -s http://127.0.0.1:$M/api/state | json 'json.dumps([d["output"]["leds"][i] for i in (0,11,32,42)]+[d["output"]["limit"]["end_lamps"]])')
wait $SP
echo "       off(5 lamps) LED 0/11/32/42: $OFF5"
echo "       on (5 lamps) LED 0/11/32/42: $ON"
check "off: LED 32 = LED 42 (lamp 5 held), end_lamps false" \
  "$(python3 -c "import json,sys;a=json.loads('$OFF5');sys.exit(0 if a[2]==a[3] and a[4] is False else 1)"; echo $?)"
check "on: LED 42 = lamp 5 (same as off), LED 32 = lamp 4 (differs), LED 0 = lamp 1, LED 11 = lamp 2" \
  "$(python3 -c "import json,sys;a=json.loads('$ON');b=json.loads('$OFF5');sys.exit(0 if a[4] is True and a[3]==b[3] and a[2]!=a[3] and a[0]==b[0] and a[1]!=a[0] else 1)"; echo $?)"

echo "== E) Restart from the page =="
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://$LAN:$M/api/restart)
check "restart from the LAN refused ($C)" "$([ "$C" = 403 ]; echo $?)"
check "state: restart_available (no systemd unit)" "$(curl -s http://127.0.0.1:$M/api/state | json 'd["restart_available"]' | grep -q True; echo $?)"
rm -f "$B/ambi_bridge"; printf '#!/bin/sh\nexit 3\n' > "$B/ambi_bridge"; chmod +x "$B/ambi_bridge"
C=$(post_local /api/restart '' | json 'd.get("error")')
sleep 1
check "broken build: refused ($C), the same bridge keeps running" \
  "$([ "$C" = binary_failed ] && kill -0 $BP 2>/dev/null && curl -sf http://127.0.0.1:$R/api/config >/dev/null; echo $?)"
rm -f "$B/ambi_bridge"; cp "$SRC_BIN" "$B/ambi_bridge"
N0=$(grep -c "ambi-bridge-cpp $VER starting" "$S.log")
C=$(post_local /api/restart '' | json 'd.get("ok")')
sleep 3
N1=$(grep -c "ambi-bridge-cpp $VER starting" "$S.log")
check "restart: ok=$C, same pid $BP, started again ($N0 -> $N1), REST + monitor answer" \
  "$([ "$C" = True ] && [ "$N1" = $((N0+1)) ] && kill -0 $BP 2>/dev/null && curl -sf http://127.0.0.1:$R/api/config >/dev/null && curl -sf http://127.0.0.1:$M/api/state >/dev/null; echo $?)"
check "log: restart requested from monitor page (old -> new version)" \
  "$(grep -q "restart requested from monitor page: ambi-bridge-cpp $VER -> ambi-bridge-cpp $VER" "$S.log"; echo $?)"
check "after restart: same pairing (known user)" "$(curl -s http://127.0.0.1:$R/api/$U/lights | python3 -c 'import sys,json;d=json.load(sys.stdin);sys.exit(0 if len(d)==5 else 1)'; echo $?)"
kill -INT $BP; sleep 1.5; kill -9 $BP 2>/dev/null

echo "== F) Restart from the terminal (r twice within 3 s) =="
S2="$S"2
( sleep 3; printf r; sleep 3.4; printf r; sleep 1; printf 'ρ'; sleep 0.4; printf 'ρ'; sleep 5 ) | \
  script -qec "stty cols 110 rows 30; exec $B/ambi_bridge --state-dir $S2 --port $R --stream-port $D --monitor-port $M --pairing auto" /dev/null > /dev/null 2>&1 &
SCP=$!
sleep 11
L1="$S2/ambi_bridge.log.1"; L="$S2/ambi_bridge.log"
check "first r, then r 3.4 s later: no restart, asked twice" "$([ "$(grep -c 'press r again within 3 s' "$L1")" -ge 2 ]; echo $?)"
check "ρ ρ (Greek layout): restart from the terminal, the new run has its own log" \
  "$(grep -q 'restart requested from terminal' "$L1" && grep -q '=== log started' "$L" && curl -sf http://127.0.0.1:$R/api/config >/dev/null; echo $?)"
pkill -INT -f "state-dir $S2" 2>/dev/null; sleep 1.5; pkill -9 -f "state-dir $S2" 2>/dev/null; kill $SCP 2>/dev/null; wait $SCP 2>/dev/null

[ $RC = 0 ] && echo "v024_test: PASS" || echo "v024_test: FAIL"
exit $RC
