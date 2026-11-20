#!/bin/bash
# v0.15 (Phase 3) end-to-end test, no TV needed:
#  A) strip settings: the monitor's /api/settings/strip can be read from
#     anywhere but changed only by this machine's own browser (127.0.0.1 +
#     matching Host/Origin); values are clamped; changes reach settings.json.
#     /ambi/settings on the REST port answers only 127.0.0.1.
#  B) "TV wants to pair": a refused POST /api (button not pressed) sets
#     link_button.tv_waiting; Pair + retry pairs and clears it.
#  C) a real DTLS-PSK stream (tools/sender) is rendered with A's settings.
# The "from elsewhere" requests use this machine's LAN address, so the
# bridge sees a non-loopback peer.
#
# usage: tools/settings_pair_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains
# "state-dir /tmp/e2e_set" (the pkill below would kill that shell).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
LAN=$(hostname -I | awk '{print $1}')
M=18110; R=18092; S=/tmp/e2e_set
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
RC=0
check() {  # $1 = description, $2 = condition result (0 = ok)
  if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi
}
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }

pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S"
"$BIN" --state-dir "$S" --port $R --stream-port 12112 --monitor-port $M --console quiet --pairing button > "$S.log" 2>&1 &
BP=$!; sleep 1.5

echo "== A) strip settings =="
G=$(curl -s http://127.0.0.1:$M/api/settings/strip)
check "GET from 127.0.0.1: can_edit=true, budget default 450" \
  "$([ "$(echo "$G" | json 'd["can_edit"], d["settings"]["current_budget_ma"]')" = "True 450" ]; echo $?)"
G2=$(curl -s http://$LAN:$M/api/settings/strip)
check "GET from LAN ($LAN): can_edit=false, values still visible" \
  "$([ "$(echo "$G2" | json 'd["can_edit"], d["settings"]["led_count"]')" = "False 43" ]; echo $?)"

C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://$LAN:$M/api/settings/strip \
  -H "Origin: http://$LAN:$M" -H 'Content-Type: application/json' -d '{"current_budget_ma":5}')
check "POST from LAN refused (403)" "$([ "$C" = 403 ]; echo $?)"
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:$M/api/settings/strip \
  -H 'Content-Type: application/json' -d '{"current_budget_ma":5}')
check "POST from 127.0.0.1 without Origin refused (403)" "$([ "$C" = 403 ]; echo $?)"
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:$M/api/settings/strip \
  -H "Origin: http://evil.example" -H 'Content-Type: application/json' -d '{"current_budget_ma":5}')
check "POST from 127.0.0.1 with a foreign Origin refused (403)" "$([ "$C" = 403 ]; echo $?)"

P=$(curl -s -X POST http://127.0.0.1:$M/api/settings/strip -H "Origin: http://127.0.0.1:$M" \
  -H 'Content-Type: application/json' \
  -d '{"ma_per_led_full_white":38.5,"current_budget_ma":700,"led_count":44,"idle_ma_per_led":0.8,"peak_emphasis":3,"rise_ms":-5}')
check "POST from the local page applies + clamps (pe 3->1, rise -5->0)" \
  "$([ "$(echo "$P" | json 'd["saved_to_disk"], d["settings"]["ma_per_led_full_white"], d["settings"]["current_budget_ma"], d["settings"]["led_count"], d["settings"]["idle_ma_per_led"], d["settings"]["peak_emphasis"], d["settings"]["rise_ms"]')" = "True 38.5 700 44 0.8 1 0" ]; echo $?)"
check "settings.json on disk has the new values" \
  "$([ "$(json 'd["current_budget_ma"], d["led_count"]' < "$S/settings.json")" = "700 44" ]; echo $?)"

C=$(curl -s -o /dev/null -w '%{http_code}' http://$LAN:$R/ambi/settings)
check "REST /ambi/settings from LAN refused (403)" "$([ "$C" = 403 ]; echo $?)"
C=$(curl -s -o /dev/null -w '%{http_code}' -X PUT http://$LAN:$R/ambi/settings -d '{"current_budget_ma":5}')
check "REST PUT /ambi/settings from LAN refused (403)" "$([ "$C" = 403 ]; echo $?)"
L=$(curl -s http://127.0.0.1:$R/ambi/settings)
check "REST /ambi/settings from 127.0.0.1 still works (budget 700)" \
  "$([ "$(echo "$L" | json 'd["current_budget_ma"]')" = "700" ]; echo $?)"

echo
echo "== B) TV wants to pair =="
W=$(curl -s http://127.0.0.1:$M/api/state | json 'd["link_button"]["tv_waiting"]')
check "at start: tv_waiting=false" "$([ "$W" = False ]; echo $?)"
E=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#pair","generateclientkey":true}' | json 'd[0]["error"]["type"]')
check "POST /api without the button -> error 101" "$([ "$E" = 101 ]; echo $?)"
W=$(curl -s http://127.0.0.1:$M/api/state | json 'd["link_button"]["tv_waiting"], d["link_button"]["refused_ago_ms"] is not None')
check "after the refusal: tv_waiting=true" "$([ "$W" = "True True" ]; echo $?)"
curl -s -X POST http://127.0.0.1:$M/api/pair >/dev/null
W=$(curl -s http://127.0.0.1:$M/api/state | json 'd["link_button"]["tv_waiting"], d["link_button"]["remaining_ms"] > 0')
check "Pair pressed: window open, still waiting for the TV" "$([ "$W" = "True True" ]; echo $?)"
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#pair","generateclientkey":true}')
USER=$(echo "$RESP" | json 'd[0]["success"]["username"]'); KEY=$(echo "$RESP" | json 'd[0]["success"]["clientkey"]')
check "TV retries: pairing succeeds" "$([ ${#USER} = 32 ]; echo $?)"
W=$(curl -s http://127.0.0.1:$M/api/state | json 'd["link_button"]["tv_waiting"], d["link_button"]["paired_ago_ms"] is not None and d["link_button"]["paired_ago_ms"] < 15000')
check "after pairing: tv_waiting=false, paired_ago_ms recent (page says Συνδέθηκε)" "$([ "$W" = "False True" ]; echo $?)"

echo
echo "== C) a real DTLS stream renders with the settings changed in A =="
curl -s -X PUT http://127.0.0.1:$R/api/$USER/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$USER" "$KEY" 12112 2 >/dev/null
O=$(curl -s http://127.0.0.1:$M/api/state)
check "output: 44 LEDs, budget 700, 38.5 mA/LED, estimate <= 700" \
  "$([ "$(echo "$O" | json 'len(d["output"]["leds"]), d["output"]["budget_ma"], d["output"]["ma_per_led_full_white"], d["output"]["estimated_ma"] <= 700')" = "44 700 38.5 True" ]; echo $?)"
grep -c "pairing refused: link button not pressed" "$S.log" | sed 's/^/       log lines "pairing refused": /'

kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null
echo
[ $RC = 0 ] && echo "settings_pair_test: PASS" || echo "settings_pair_test: FAIL"
exit $RC
