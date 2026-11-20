#!/bin/bash
# v0.44: «Διάγνωση», «Καταγραφή Hue» and the night limit, against the simulator (build/esp_app_sim
# = the firmware's App code on Linux). Loopback only, test ports, nothing on the LAN.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18496; SPORT=21906; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
st() { curl -s -m 3 $H/ambi/api/state; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
start_sim() { "$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!; sleep 0.8; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
start_sim

check "diag: App counters present, nothing paired yet" "[ \"\$(curl -s $H/ambi/api/diag | js 'd[\"app\"][\"paired\"],d[\"app\"][\"hs_ok\"],d[\"app\"][\"capture_on\"],\"fps\" in d[\"app\"]')\" = '0 0 False True' ]"
curl -s -XPOST -d '{"action":"start"}' $H/ambi/api/service > /dev/null
curl -s -XPOST $H/sim/press > /dev/null
RESP=$(curl -s -XPOST $H/api -d '{"devicetype":"v044#test","generateclientkey":true}')
U=$(echo "$RESP" | js 'd[0]["success"]["username"]'); K=$(echo "$RESP" | js 'd[0]["success"]["clientkey"]')
G=$(curl -s -XPOST $H/api/$U/groups -d '{"lights":["1","2","3"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
curl -s -XPOST -d '{"on":true}' $H/ambi/api/capture > /dev/null
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=3 "$T/sender" "$U" "$K" $SPORT 6 > "$D/sender.log" 2>&1 & SP=$!
sleep 2.5
check "diag while streaming: paired 1, handshake ok, fps > 15, frame age < 1 s" "[ \"\$(curl -s $H/ambi/api/diag | js 'd[\"app\"][\"paired\"],d[\"app\"][\"hs_ok\"]>=1,d[\"app\"][\"fps\"]>15,d[\"app\"][\"frame_age_s\"]<1')\" = '1 True True True' ]"
ROWS=$(curl -s "$H/ambi/api/capture?from=0")
check "capture: rows with 3 raw lamps (16-bit values), increasing seq" "[ \"\$(echo '$ROWS' | js 'len(d[\"rows\"])>10,d[\"rows\"][0][3],max(d[\"rows\"][-1][4:13])>255,d[\"rows\"][-1][0]>d[\"rows\"][0][0]')\" = 'True 3 True True' ]"
NEXT=$(echo "$ROWS" | js 'd["next"]'); sleep 1
check "capture: ?from=next returns only new rows" "[ \"\$(curl -s \"$H/ambi/api/capture?from=$NEXT\" | js 'all(r[0]>$NEXT for r in d[\"rows\"]) and len(d[\"rows\"])>0')\" = True ]"
MA0=$(st | js 'round(d["output"]["estimated_ma"] or 0)')
# the simulator gives the App the plain epoch (UTC); on the ESP it is WLED's local time
HOUR=$(date -u +%H | sed 's/^0//'); TO=$(( (HOUR + 1) % 24 ))
curl -s -XPOST -d "{\"night_on\":true,\"night_from\":$HOUR,\"night_to\":$TO,\"night_pct\":20}" $H/ambi/api/settings/device > /dev/null
sleep 1.5
MA1=$(st | js 'round(d["output"]["estimated_ma"] or 0)')
check "night limit now (20 %): the current falls (${MA0} -> ${MA1} mA)" "[ $MA1 -lt $((MA0 * 6 / 10)) ]"
check "diag: night_now while in the window" "[ \"\$(curl -s $H/ambi/api/diag | js 'd[\"app\"][\"night_now\"]')\" = True ]"
curl -s -XPOST -d '{"night_on":false}' $H/ambi/api/settings/device > /dev/null
check "device settings carry the night limit" "[ \"\$(curl -s $H/ambi/api/settings/device | js 'd[\"night_on\"],d[\"night_pct\"]')\" = 'False 20' ]"
wait $SP
curl -s -XPOST -d '{"on":false}' $H/ambi/api/capture > /dev/null
check "capture off: no rows kept on the ESP" "[ \"\$(curl -s \"$H/ambi/api/capture?from=0\" | js 'd[\"on\"],len(d[\"rows\"])')\" = 'False 0' ]"
P="$R/firmware/wled-usermod/AmbiBottom/ambi_page.html"
check "page: diagnosis, capture, night fields, wear details folded; v0.47: wear panel in the side column after the current" "grep -q 'id=\"espDiagPanel\"' '$P' && grep -q 'id=\"espCapPanel\"' '$P' && grep -q 'id=\"devNightOn\"' '$P' && grep -q 'id=\"logMore\"' '$P' && python3 -c \"import sys;p=open('$P').read();a=p.index('<aside class=\\\"side-panel\\\">');w=p.index('id=\\\"stripLogPanel\\\"');sys.exit(0 if a<w<p.index('</aside>',a) and p.index('id=\\\"outMore\\\"')<w<p.index('id=\\\"espPanel\\\"') else 1)\""

kill $SIMPID 2>/dev/null; wait $SIMPID 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
