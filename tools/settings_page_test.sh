#!/bin/bash
# v0.32 end to end, no TV: the monitor's settings page /ambi/settings and its
# save route POST /api/settings/strip/validated - defaults, who may save,
# wrong values refused with nothing saved, good values in settings.json, and
# the new settings acting on a real DTLS stream (brightness, lamp positions).
# Expect 19 [ok] and PASS. SSDP/mDNS are off (AMBI_NO_DISCOVERY).
# Don't launch it from a shell whose own command line contains "state-dir /tmp/e2e_setpage".
#
# usage: tools/settings_page_test.sh [bridge binary]
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
export AMBI_NO_DISCOVERY=1
LAN=$(hostname -I | awk '{print $1}')
M=18639; R=18631; S=/tmp/e2e_setpage; SP=12631
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
OKS=0; FAILS=0
check() { if eval "$2"; then OKS=$((OKS+1)); echo "[ok]   $1"; else FAILS=$((FAILS+1)); echo "[FAIL] $1"; fi; }
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
post() {  # $1 = body; prints "<http code> <body>"
  curl -s -w '\n%{http_code}' -X POST http://127.0.0.1:$M/api/settings/strip/validated -H "Origin: http://127.0.0.1:$M" \
    -H 'Content-Type: application/json' -d "$1" | python3 -c 'import sys;t=sys.stdin.read().rsplit("\n",1);print(t[1],t[0])'
}
leds() { curl -s http://127.0.0.1:$M/api/state | json "$1"; }
pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.4; rm -rf "$S"
"$BIN" --state-dir "$S" --port $R --stream-port $SP --monitor-port $M --console quiet --pairing auto > "$S.log" 2>&1 &
BP=$!; sleep 1.5

echo "== the page =="
P=$(curl -s -w '\n%{http_code} %{content_type}' http://127.0.0.1:$M/ambi/settings)
check "GET /ambi/settings: 200, HTML" "echo \"\$P\" | tail -1 | grep -q '^200 text/html'"
check "it has every field asked for (LED count, mA per LED, current limit, brightness, rise_ms, peak_emphasis, lamp positions)" \
  "for f in 'id=\"led_count\"' 'id=\"ma_per_led_full_white\"' 'id=\"current_budget_ma\"' 'id=\"brightness\"' 'id=\"rise_ms\"' 'id=\"peak_emphasis\"' 'id=\"strip_layout\"' 'id=\"lamps\"'; do echo \"\$P\" | grep -q \"\$f\" || exit 1; done"
check "the main page links to it" "curl -s http://127.0.0.1:$M/ | grep -q 'href=\"/ambi/settings\"'"
check "also readable from the LAN ($LAN)" "[ \$(curl -s -o /dev/null -w '%{http_code}' http://$LAN:$M/ambi/settings) = 200 ]"

echo "== defaults and who may save =="
G=$(curl -s http://127.0.0.1:$M/api/settings/strip)
check "defaults: 43 LEDs, 45 mA per LED, 450 mA limit, brightness 1, rise 120 ms, peak_emphasis 0.5, layout tv" \
  "[ \"\$(echo '$G' | json 'd[\"settings\"][\"led_count\"], d[\"settings\"][\"ma_per_led_full_white\"], d[\"settings\"][\"current_budget_ma\"], d[\"settings\"][\"brightness\"], d[\"settings\"][\"rise_ms\"], d[\"settings\"][\"peak_emphasis\"], d[\"settings\"][\"strip_layout\"], d[\"settings\"][\"lamp_positions\"]')\" = '43 45 450 1 120 0.5 tv []' ]"
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://$LAN:$M/api/settings/strip/validated -H "Origin: http://$LAN:$M" -H 'Content-Type: application/json' -d '{"led_count":10}')
check "save from the LAN without login: refused (403)" "[ $C = 403 ]"
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:$M/api/settings/strip/validated -H "Origin: http://evil.example" -H 'Content-Type: application/json' -d '{"led_count":10}')
check "save with a foreign Origin (another site's form): refused (403)" "[ $C = 403 ]"
check "not JSON: 400" "[ \"\$(post 'led_count=10' | cut -d' ' -f1)\" = 400 ]"

echo "== wrong values: refused, nothing saved =="
B=$(post '{"led_count":0,"current_budget_ma":450,"brightness":3,"rise_ms":"fast","lamp_positions":[0,2,1]}')
check "422 and every wrong field named (led_count, brightness, rise_ms, lamp_positions), the good one not" \
  "[ \"\${B%% *}\" = 422 ] && [ \"\$(echo \"\${B#* }\" | json 'sorted(e[\"field\"] for e in d[\"errors\"])')\" = \"['brightness', 'lamp_positions', 'led_count', 'rise_ms']\" ]"
check "nothing changed and no settings.json was written" \
  "[ \"\$(curl -s http://127.0.0.1:$M/api/settings/strip | json 'd[\"settings\"][\"led_count\"], d[\"settings\"][\"current_budget_ma\"]')\" = '43 450' ] && [ ! -e $S/settings.json ]"
check "a typo in a name is refused too" "[ \"\$(post '{\"curent_budget_ma\":300}' | cut -d' ' -f1)\" = 422 ]"

echo "== good values: saved and in force =="
B=$(post '{"led_count":30,"ma_per_led_full_white":55,"current_budget_ma":900,"brightness":1,"rise_ms":0,"peak_emphasis":0.3}')
check "200, saved_to_disk, values echoed" "[ \"\${B%% *}\" = 200 ] && [ \"\$(echo \"\${B#* }\" | json 'd[\"saved_to_disk\"], d[\"settings\"][\"led_count\"], d[\"settings\"][\"peak_emphasis\"]')\" = 'True 30 0.3' ]"
check "settings.json has them" "[ \"\$(json 'd[\"led_count\"], d[\"ma_per_led_full_white\"], d[\"current_budget_ma\"], d[\"rise_ms\"], d[\"peak_emphasis\"]' < $S/settings.json)\" = '30 55 900 0 0.3' ]"

echo "== acting on a real stream (tools/sender: 3 lamps, a still picture) =="
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#setpage","generateclientkey":true}')
U=$(echo "$RESP" | json 'd[0]["success"]["username"]'); K=$(echo "$RESP" | json 'd[0]["success"]["clientkey"]')
curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$U" "$K" $SP 9 >/dev/null & SNDR=$!
sleep 2
FULL=$(leds 'len(d["output"]["leds"]), d["output"]["leds"][0], d["output"]["leds"][-1]')
check "30 LEDs on the strip now" "[ \"\${FULL%% *}\" = 30 ]"
A0=$(leds 'd["output"]["leds"][0]'); A9=$(leds 'd["output"]["leds"][-1]')
post '{"brightness":0.5}' >/dev/null; sleep 0.6
check "brightness 0.5: every LED about half of before" \
  "curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;h=json.load(sys.stdin)[\"output\"][\"leds\"];a=$A0;b=$A9;ok=all(abs(h[0][i]-a[i]/2)<=1.5 for i in range(3)) and all(abs(h[-1][i]-b[i]/2)<=1.5 for i in range(3));sys.exit(0 if ok else 1)'"
post '{"brightness":1,"strip_layout":"custom","lamp_positions":[1,0.5,0]}' >/dev/null; sleep 0.6
check "custom positions reversed: the strip is mirrored (LED 0 now shows what the last LED showed)" \
  "curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;h=json.load(sys.stdin)[\"output\"][\"leds\"];a=$A0;b=$A9;ok=all(abs(h[0][i]-b[i])<=2 for i in range(3)) and all(abs(h[-1][i]-a[i])<=2 for i in range(3));sys.exit(0 if ok else 1)'"
post '{"lamp_positions":[0,-1,1]}' >/dev/null; sleep 0.6
check "lamp 2 switched off in the map: saved as -1, strip still lit end to end" \
  "[ \"\$(json 'd[\"lamp_positions\"]' < $S/settings.json)\" = '[0, -1, 1]' ] && curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;h=json.load(sys.stdin)[\"output\"][\"leds\"];sys.exit(0 if sum(h[0])>0 and sum(h[-1])>0 else 1)'"
post '{"current_budget_ma":100,"strip_layout":"tv"}' >/dev/null; sleep 0.6
check "current limit 100 mA: the estimate stays within it" "python3 -c 'import sys;sys.exit(0 if $(leds 'd["output"]["estimated_ma"]') <= 100.01 else 1)'"
kill $SNDR 2>/dev/null

echo "== restart: the saved settings come back =="
kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null
"$BIN" --state-dir "$S" --port $R --stream-port $SP --monitor-port $M --console quiet --pairing auto >> "$S.log" 2>&1 &
BP=$!; sleep 1.5
check "after a restart: 30 LEDs, 100 mA, lamp_positions [0, -1, 1]" \
  "[ \"\$(curl -s http://127.0.0.1:$M/api/settings/strip | json 'd[\"settings\"][\"led_count\"], d[\"settings\"][\"current_budget_ma\"], d[\"settings\"][\"lamp_positions\"]')\" = '30 100 [0, -1, 1]' ]"
kill -INT $BP; sleep 0.8; kill -9 $BP 2>/dev/null
echo; echo "$OKS [ok], $FAILS failed"; [ $FAILS = 0 ] && echo "settings_page_test: PASS" || echo "settings_page_test: FAILED (log: $S.log)"
exit $FAILS
