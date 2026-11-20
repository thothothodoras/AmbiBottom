#!/bin/bash
# v0.35 end to end: the debug colour trace (--color-trace) on the REAL path:
# real DTLS stream (tools/sender, 5 XY lamps) + mock JointSpace TV (tools/mock_tv.py) ->
# bridge --color-trace --wled-ip -> tools/mock_wled.py.
#  A) version v0.35; without --color-trace no trace file appears
#  B) every line is JSON; the lamps' raw values are the sender's bytes exactly
#  C) rgb = the bridge's xy -> RGB of those raw values (re-computed here in Python), led = its linear value
#  D) js = the mock TV's processed sides (3 / 8 / 3 zones, the same as /api/state)
#  E) ddp = the bytes the (mock) WLED really received
#  F) the trace stops by itself after --color-trace-seconds and says so in the log
# usage: tools/v035_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains "state-dir /tmp/e2e_v35",
# "mock_tv.py 1926" or "mock_wled.py 18644" (pkill below). Not together with other mock-TV tests.
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
M=18647; R=18642; D=12644; W=14644; WH=18644; S=/tmp/e2e_v35
TR=$S.trace.jsonl
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1

pkill -9 -f "state-dir $S" 2>/dev/null; pkill -f "mock_tv.py 1926" 2>/dev/null; pkill -f "mock_wled.py $WH" 2>/dev/null
sleep 0.5; rm -rf "$S" "$TR"
V=$("$BIN" --version 2>&1 | head -1)
check "A version line says v0.35 or later ($V)" "$(echo "$V" | grep -Eq 'v0\.(3[5-9]|[4-9][0-9])'; echo $?)"

python3 "$T/mock_tv.py" 1926 > "$S.mock.log" 2>&1 &
MP=$!
python3 "$T/mock_wled.py" $WH $W --leds 43 --ledma 45 --maxpwr 570 > "$S.wled.log" 2>&1 &
WP=$!; sleep 0.8
AMBI_NO_DISCOVERY=1 "$BIN" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet --pairing auto --lamps 5 \
  --tv-ip 127.0.0.1 --tv-user tvuser --tv-pass tvpass --js-interval 0.2 \
  --wled-ip 127.0.0.1 --wled-port $W --color-trace "$TR" --color-trace-seconds 9 > "$S.log" 2>&1 &
BP=$!; sleep 3
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#v35","generateclientkey":true}')
U=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])')
K=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])')
curl -s -X PUT http://127.0.0.1:$R/ambi/settings -d '{"rise_ms":0,"peak_drift_period_s":0}' >/dev/null
curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
SENDER_LAMPS=5 "$T/sender" "$U" "$K" $D 20 >/dev/null 2>&1 &
SP=$!; sleep 3

STATE=$(curl -s http://127.0.0.1:$M/api/state)
LIVE=$(curl -s http://127.0.0.1:$WH/json/live)
sleep 6   # past --color-trace-seconds 9 (counted from the bridge's start)
N1=$(wc -l < "$TR"); sleep 1.5; N2=$(wc -l < "$TR")

OUT=$(python3 - "$TR" "$STATE" "$LIVE" <<'EOF'
import sys, json, math
lines = [json.loads(l) for l in open(sys.argv[1])]
state = json.loads(sys.argv[2]); live = json.loads(sys.argv[3])["leds"]
def xy_bri_to_rgb(x, y, bri):           # copy of color_pipeline.cpp xy_bri_to_rgb
    if y <= 0: return (0, 0, 0)
    Y = max(0.0, min(1.0, bri / 254.0)); X = Y / y * x; Z = Y / y * (1 - x - y)
    r = X * 1.656492 - Y * 0.354851 - Z * 0.255038
    g = -X * 0.707196 + Y * 1.655397 + Z * 0.036152
    b = X * 0.051713 - Y * 0.121364 + Z * 1.011530
    gm = lambda c: 12.92 * max(c, 0) if max(c, 0) <= 0.0031308 else 1.055 * max(c, 0) ** (1 / 2.4) - 0.055
    r, g, b = gm(r), gm(g), gm(b); m = max(r, g, b, 1.0)
    return tuple(int(max(0, min(255, math.floor(255 * c / m + 0.5)))) for c in (r, g, b))
lin = lambda v: round(255 * ((v / 255) / 12.92 if v / 255 <= 0.04045 else ((v / 255 + 0.055) / 1.055) ** 2.4))
sent = {"1": (0xB000, 0x5000, 0x8000), "2": (0x6000, 0x4000, 0x8000), "3": (0x3000, 0x9000, 0x8000),
        "4": (0x2000, 0x1800, 0x8000), "5": (0x5000, 0x5500, 0xFFFF)}
raw_ok = all(len(f["lamps"]) == 5 and all(tuple(l["raw"]) == sent[l["id"]] for l in f["lamps"]) and f["cs"] == 1 for f in lines)
rgb_ok = all(all(max(abs(a - b) for a, b in zip(l["rgb"], xy_bri_to_rgb(l["raw"][0] / 65535, l["raw"][1] / 65535, l["raw"][2] / 65535 * 254))) <= 1
                 and tuple(l["led"]) == tuple(lin(c) for c in l["rgb"]) for l in f["lamps"]) for f in lines)
js_lines = [f for f in lines if f["js"]["left"]]
last = js_lines[-1]["js"] if js_lines else {}
js_ok = bool(js_lines) and (len(last["left"]), len(last["top"]), len(last["right"])) == (3, 8, 3) \
        and [list(z) for z in state["sides"]["left"]] == last["left"]
want = "".join(live)  # mock WLED's received pixels as RRGGBB per LED
ddp_ok = any(f["ddp"] == want.lower() and f["ddp_sent"] for f in lines)  # frames change (peaks drift): any line
print(len(lines), raw_ok, rgb_ok, js_ok, ddp_ok, len(js_lines))
EOF
)
echo "       lines, raw, rgb, js, ddp, lines with js = $OUT"
read -r NL RAW RGB JS DDP NJS <<< "$OUT"
check "B $NL JSON lines; raw values = the sender's bytes (x16, y16, bri16) on every line" "$([ "$NL" -gt 20 ] && [ "$RAW" = True ]; echo $?)"
check "C rgb = xy -> RGB of the raw values (Python copy, within 1); led = its linear value" "$([ "$RGB" = True ]; echo $?)"
check "D js = the mock TV's processed sides, 3 / 8 / 3 zones, same as /api/state ($NJS lines with js)" "$([ "$JS" = True ]; echo $?)"
check "E ddp = the bytes the mock WLED received" "$([ "$DDP" = True ]; echo $?)"
check "F trace stopped by itself ($N1 -> $N2 lines while the stream still ran) and logged it" \
  "$([ "$N1" = "$N2" ] && grep -q 'colour trace finished' "$S.log"; echo $?)"

kill $SP 2>/dev/null; kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null

# A2) no flag, no file
rm -f "$S.notrace"; AMBI_NO_DISCOVERY=1 "$BIN" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet > "$S.log2" 2>&1 &
BP=$!; sleep 2; kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null
check "A without --color-trace: no trace line in the log" "$(! grep -q '\[trace\]' "$S.log2"; echo $?)"

kill $MP $WP 2>/dev/null
echo
[ $RC = 0 ] && echo "v035_test: PASS" || echo "v035_test: FAIL"
exit $RC
