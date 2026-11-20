#!/bin/bash
# v0.42: soft on / off and the security card, against the simulator (build/esp_app_sim = the
# firmware's App code on Linux). Loopback only, test ports, nothing on the LAN.
# Checks: fade_in_ms / fade_out_ms are strip settings (defaults 800 / 1500, saved, 0..10000);
# Start while the TV streams ramps up (not at once); the strip fades when the stream is ended
# on purpose (stream.active=false: no 1 s hold); the security card answers and the "Hue only
# from the TV" switch needs the TV's address first.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18494; SPORT=21904; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
st() { curl -s -m 3 $H/ambi/api/state; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
ma() { st | js 'round(d["output"]["estimated_ma"] or 0)'; }
start_sim() { "$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!; sleep 0.8; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
start_sim

check "fade_in_ms 800 / fade_out_ms 1500 by default" "[ \"\$(curl -s $H/ambi/api/settings/strip | js 'd[\"settings\"][\"fade_in_ms\"],d[\"settings\"][\"fade_out_ms\"]')\" = '800 1500' ]"
check "fade_in_ms 20000 -> clamped to 10000 (as the ESP does for every strip setting)" "[ \"\$(curl -s -XPOST -d '{\"fade_in_ms\":20000}' $H/ambi/api/settings/strip | js 'd[\"settings\"][\"fade_in_ms\"]')\" = 10000 ]"
curl -s -XPOST -d '{"fade_in_ms":1500}' $H/ambi/api/settings/strip > /dev/null
check "fade_in_ms saved (1500)" "[ \"\$(curl -s $H/ambi/api/settings/strip | js 'd[\"settings\"][\"fade_in_ms\"]')\" = 1500 ]"
check "security card: no TV seen yet -> the switch is refused (409)" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -XPOST -d '{\"hue_only_tv\":true}' $H/ambi/api/security)\" = 409 ]"

# the TV streams while stopped, then Start: the strip ramps up over fade_in_ms (1.5 s), not at once
curl -s -XPOST $H/sim/press > /dev/null
RESP=$(curl -s -XPOST $H/api -d '{"devicetype":"v042#test","generateclientkey":true}')
U=$(echo "$RESP" | js 'd[0]["success"]["username"]'); K=$(echo "$RESP" | js 'd[0]["success"]["clientkey"]')
G=$(curl -s -XPOST $H/api/$U/groups -d '{"lights":["1","2","3"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=3 "$T/sender" "$U" "$K" $SPORT 8 > "$D/sender.log" 2>&1 & SP=$!
sleep 2.5
curl -s -XPOST -d '{"action":"start"}' $H/ambi/api/service > /dev/null
sleep 0.3; A=$(ma); sleep 2.5; B=$(ma)
check "Start while streaming: ramps up (${A} mA after 0.3 s, ${B} mA after 2.8 s)" "[ $B -gt 0 ] && [ $A -lt $((B * 6 / 10)) ]"
check "security card: TV seen now -> Hue only from it" "[ \"\$(curl -s -XPOST -d '{\"hue_only_tv\":true}' $H/ambi/api/security | js 'd[\"hue_only_ip\"]')\" = 127.0.0.1 ]"
curl -s -XPOST -d '{"hue_only_tv":false}' $H/ambi/api/security > /dev/null
wait $SP
check "the page has the security card and the two fade settings" "grep -q 'id=\"espSecPanel\"' '$R/firmware/wled-usermod/AmbiBottom/ambi_page.html' && grep -q 'id=\"setFadeIn\"' '$R/firmware/wled-usermod/AmbiBottom/ambi_page.html' && grep -q 'id=\"stripSettingsPanel\" ' '$R/firmware/wled-usermod/AmbiBottom/ambi_page.html'"

# v0.43: the Demo exists only with its option on
curl -s -XPOST -d '{"demo_enabled":false}' $H/ambi/api/settings/device > /dev/null
check "v0.43: Demo option off -> starting the demo is refused (409)" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -XPOST -d '{\"on\":true}' $H/ambi/api/demo)\" = 409 ]"
curl -s -XPOST -d '{"demo_enabled":true}' $H/ambi/api/settings/device > /dev/null
check "v0.43: Demo option on -> the demo starts" "[ \"\$(curl -s -XPOST -d '{\"on\":true}' $H/ambi/api/demo | js 'd[\"on\"]')\" = True ]"
check "v0.43: page has the Demo option and the PIN pill" "grep -q 'id=\"devDemo\"' '$R/firmware/wled-usermod/AmbiBottom/ambi_page.html' && grep -q 'id=\"pinPill\"' '$R/firmware/wled-usermod/AmbiBottom/ambi_page.html'"
kill $SIMPID 2>/dev/null; wait $SIMPID 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
