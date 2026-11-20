#!/bin/bash
# v0.41: the ESP page's new parts, against the simulator (build/esp_app_sim = the firmware's App
# code on Linux). Loopback only, test ports, nothing on the LAN.
# Checks: the LED wear log on the ESP (GET /ambi/api/strip_stats counts a stream, "since
# install" empty until marked, then counts; reset clears only since_reset; saved through the
# platform's file and loaded again after a restart); the demo comes back ~2 s after a stream
# ends (not after loss_black_s); the device settings carry name / button / time; bad input 422.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18493; SPORT=21903; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
st() { curl -s -m 3 $H/ambi/api/state; }
ss() { curl -s -m 3 $H/ambi/api/strip_stats; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
start_sim() { "$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!; sleep 0.8; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
start_sim

# stopped: the TV's frames are not on the strip -> not counted
curl -s -XPOST $H/sim/press > /dev/null
RESP0=$(curl -s -XPOST $H/api -d '{"devicetype":"v041#stopped","generateclientkey":true}')
U0=$(echo "$RESP0" | js 'd[0]["success"]["username"]'); K0=$(echo "$RESP0" | js 'd[0]["success"]["clientkey"]')
G0=$(curl -s -XPOST $H/api/$U0/groups -d '{"lights":["1","2","3"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
curl -s -XPUT $H/api/$U0/groups/$G0 -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=3 "$T/sender" "$U0" "$K0" $SPORT 2 > "$D/sender0.log" 2>&1
curl -s -XPUT $H/api/$U0/groups/$G0 -d '{"stream":{"active":false}}' > /dev/null
sleep 1
check "wear log available, empty, not marked as mounted" "[ \"\$(ss | js 'd[\"available\"],d[\"all_time\"][\"frames\"],d[\"installed\"]')\" = 'True 0 False' ]"
curl -s -XPOST -d '{"action":"start"}' $H/ambi/api/service > /dev/null
curl -s -XPOST -d '{"on":true}' $H/ambi/api/demo > /dev/null
curl -s -XPOST $H/sim/press > /dev/null
RESP=$(curl -s -XPOST $H/api -d '{"devicetype":"v041#test","generateclientkey":true}')
U=$(echo "$RESP" | js 'd[0]["success"]["username"]'); K=$(echo "$RESP" | js 'd[0]["success"]["clientkey"]')
G=$(curl -s -XPOST $H/api/$U/groups -d '{"lights":["1","2","3"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=3 "$T/sender" "$U" "$K" $SPORT 5 > "$D/sender.log" 2>&1
END=$(date +%s.%N)
check "stream counted: since_reset ~5 s, 43 LEDs, mAh > 0" "[ \"\$(ss | js 'round(d[\"since_reset\"][\"stream_s\"])>=4,len(d[\"all_time\"][\"mah\"]),sum(d[\"all_time\"][\"mah\"])>0')\" = 'True 43 True' ]"
check "not marked: since_install empty, wear empty" "[ \"\$(ss | js 'd[\"since_install\"][\"frames\"],len(d[\"wear\"])')\" = '0 0' ]"
check "a day is counted (the simulator has a clock)" "[ \"\$(ss | js 'len(d[\"days\"]),d[\"days\"][0][\"stream_s\"]>=4')\" = '1 True' ]"
# v0.43: the TV streamed -> the demo switched itself off and does not come back (was: back after ~2 s)
sleep 5
check "demo off after the TV streamed (v0.43), strip not showing it" "[ \"\$(st | js 'd[\"demo\"],d[\"esp_demo\"][\"on\"]')\" = 'False False' ]"

curl -s -XPOST -d '{}' $H/ambi/api/strip_stats/install > /dev/null
check "marked as mounted: installed, since set" "[ \"\$(ss | js 'd[\"installed\"],bool(d[\"since_install\"][\"since\"])')\" = 'True True' ]"
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=3 "$T/sender" "$U" "$K" $SPORT 3 > "$D/sender2.log" 2>&1
check "after marking: since_install counts the new stream, wear has 43 values" "[ \"\$(ss | js 'd[\"since_install\"][\"stream_s\"]>=2,d[\"since_install\"][\"stream_s\"]<d[\"all_time\"][\"stream_s\"],len(d[\"wear\"])')\" = 'True True 43' ]"
curl -s -XPOST -d '{}' $H/ambi/api/strip_stats/reset > /dev/null
check "reset: since_reset 0, all_time and since_install kept" "[ \"\$(ss | js 'd[\"since_reset\"][\"frames\"],d[\"all_time\"][\"frames\"]>0,d[\"since_install\"][\"frames\"]>0')\" = '0 True True' ]"
check "saved to the platform file (reset / install save at once)" "[ -s '$D/state/ambi_stats.json' ]"
ALL=$(ss | js 'd["all_time"]["frames"]')
kill $SIMPID; wait $SIMPID 2>/dev/null; start_sim
check "after a restart: the log is back (all_time frames $ALL, still marked)" "[ \"\$(ss | js 'd[\"all_time\"][\"frames\"],d[\"installed\"]')\" = '$ALL True' ]"

check "device settings carry name / button / time" "[ \"\$(curl -s $H/ambi/api/settings/device | js 'bool(d[\"name\"]),d[\"button\"] in (\"ambi\",\"wled\",\"none\"),d[\"tz\"],\"time_ok\" in d')\" = 'True True 3 True' ]"
check "bad zone -> 422" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -XPOST -d '{\"tz\":99}' $H/ambi/api/settings/device)\" = 422 ]"
P="$R/firmware/wled-usermod/AmbiBottom/ambi_page.html"
check "page: wear panel shown on the ESP (not hidden), install button, device time" "! grep -q '#stripLogPanel' '$P' && grep -q 'id=\"logInstall\"' '$P' && grep -q 'id=\"devTz\"' '$P' && grep -q 'id=\"devButton\"' '$P'"

kill $SIMPID 2>/dev/null; wait $SIMPID 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
