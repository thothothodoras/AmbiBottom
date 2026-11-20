#!/bin/bash
# The ESP32 usermod's logic, end to end on Linux: tests/esp_app_sim.cpp runs
# the firmware's own application code (ambi_bottom::App + its DTLS server)
# and this script talks to it the way the TV does (curl + tools/sender).
# Same checks as tools/v031_test.sh for the Linux bridge, plus the things only
# the ESP has: pairing only with the button, state in "NVS" across a reboot.
# Expect 30 [ok] and PASS. No SSDP socket is opened: nothing shows on the LAN.
# Don't launch it from a shell whose own command line contains "esp_app_sim /tmp/e2e_esp".
#
# usage: tools/esp_app_test.sh [build dir]
T=$(cd "$(dirname "$0")" && pwd)
B=${1:-$T/../build}
SIM=$B/esp_app_sim
[ -x "$SIM" ] || { echo "no $SIM (build first)"; exit 1; }
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
R=http://127.0.0.1:18431; SP=12431; D=/tmp/e2e_esp; LOG=/tmp/e2e_esp.log
FAILS=0; OKS=0
ok() { OKS=$((OKS+1)); echo "[ok] $1"; }
bad() { FAILS=$((FAILS+1)); echo "[FAIL] $1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
st() { curl -s $R/sim/status | python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
lock() { st '"none" if not d["tv_dtls_lock"] else d["tv_dtls_lock"]["reason"]+("+confirmed" if d["tv_dtls_lock"]["confirmed"] else "")'; }
start() { "$SIM" $D 18431 $SP 3 >> $LOG 2>&1 & SIMPID=$!; sleep 0.8; }
pkill -9 -f "esp_app_sim /tmp/e2e_esp" 2>/dev/null; sleep 0.3; rm -rf $D $LOG; mkdir -p $D
start

echo "== discovery texts and description =="
check "SSDP reply for ssdp:all: 3 answers, IpBridge, LOCATION description.xml, bridge id" "[ \$(curl -s '$R/sim/ssdp?st=ssdp:all' | sed '/==== notify/q' | grep -c '^ST: ') = 3 ] && curl -s '$R/sim/ssdp?st=ssdp:all' | grep -q 'IpBridge' && curl -s '$R/sim/ssdp' | grep -q 'LOCATION: http://127.0.0.1:18431/description.xml' && curl -s '$R/sim/ssdp' | grep -q 'hue-bridgeid: 001788FFFE'"
check "a DIAL / MediaServer search gets no answer" "[ \$(curl -s '$R/sim/ssdp?st=urn:dial-multiscreen-org:service:dial:1' | sed '/==== notify/q' | grep -c '^ST: ') = 0 ]"
check "description.xml says BSB002" "curl -s $R/description.xml | grep -q '<modelNumber>BSB002</modelNumber>'"
check "other paths are not ours (WLED's own pages on the ESP)" "[ \$(curl -s -o /dev/null -w '%{http_code}' $R/json/state) = 404 ]"

echo "== pairing only with the button =="
check "POST /api without the button: error 101" "curl -s -X POST $R/api -d '{\"devicetype\":\"tv#test\",\"generateclientkey\":true}' | grep -q '\"type\":101'"
check "status shows the TV wants to pair" "[ \$(st 'd[\"tv_wants_pair\"]') = True ]"
curl -s -X POST $R/sim/press >/dev/null
RESP=$(curl -s -X POST $R/api -d '{"devicetype":"tv#test","generateclientkey":true}')
U=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])' 2>/dev/null)
K=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])' 2>/dev/null)
check "after the button: username (32 hex) and clientkey (32 hex)" "[ \${#U} = 32 ] && [ \${#K} = 32 ]"
check "pairing the same devicetype again gives the same username" "curl -s -X POST $R/api -d '{\"devicetype\":\"tv#test\",\"generateclientkey\":true}' | grep -q \"$U\""

echo "== what the TV checks; groups =="
check "/config BSB002 + apiversion >= 1.22, /capabilities streaming.available > 0" "curl -s $R/api/$U/config | grep -q '\"modelid\":\"BSB002\"' && curl -s $R/api/$U/config | grep -q '\"apiversion\":\"1.67.0\"' && curl -s $R/api/$U/capabilities | grep -q '\"streaming\":{\"available\":1'"
check "group 1 is Entertainment; same POST is reused" "curl -s $R/api/$U/groups/1 | grep -q '\"type\":\"Entertainment\"' && curl -s -X POST $R/api/$U/groups -d '{\"lights\":[\"1\",\"2\",\"3\"],\"type\":\"Entertainment\",\"class\":\"TV\"}' | grep -q '\"id\":\"1\"'"
curl -s -X POST $R/api/$U/groups -d '{"lights":["1","2","3"],"type":"LightGroup"}' >/dev/null
curl -s -X POST $R/api/$U/groups -d '{"lights":["1"],"type":"LightGroup"}' >/dev/null
check "DELETE /groups/2: gone from the list, GET answers a placeholder, /groups/4 is an error" "curl -s -X DELETE $R/api/$U/groups/2 | grep -q deleted && ! curl -s $R/api/$U/groups/2 | grep -q error && curl -s $R/api/$U/groups/2 | grep -q '\"lights\":\[\]' && curl -s $R/api/$U/groups/4 | grep -q '\"error\"'"
WORST=0; ALL200=1
for i in 1 2 3 4 5 6 7 8 9 10; do
  O=$(curl -s -o /dev/null -w '%{http_code} %{time_total}' --max-time 2 $R/api/$U/lights/1)
  [ "${O%% *}" = 200 ] || ALL200=0
  WORST=$(python3 -c "print(max($WORST, ${O##* }))")
done
check "10x GET /lights/1: all 200, worst ${WORST}s < 0.300" "[ $ALL200 = 1 ] && python3 -c 'import sys;sys.exit(0 if $WORST < 0.3 else 1)'"
curl -s $R/api/0123456789abcdef0123456789abcdef/lights >/dev/null; curl -s $R/api/0123456789abcdef0123456789abcdef/groups/1 >/dev/null
check "unknown 32-hex username: error 1 and LOST ITS PAIRING logged once" "curl -s $R/api/0123456789abcdef0123456789abcdef/lights | grep -q '\"type\":1' && [ \$(grep -c 'LOST ITS PAIRING' $LOG) = 1 ]"

echo "== the TV's order: active=false, active=true, handshake, frames =="
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$U" "$K" $SP 8 >/dev/null & S1=$!
sleep 2
check "handshake under 3 s after stream.active=true" "python3 -c 'import sys;sys.exit(0 if 0 <= $(st 'd["handshake_latency_s"]') < 3 else 1)'"
check "live, ~25 fps, 3 lamps, no lock" "[ \"\$(st 'd[\"live\"], d[\"lamps_in_stream\"], 20 < d[\"fps\"] < 30')\" = 'True 3 True' ] && [ \$(lock) = none ]"
check "43 LED values, not all black, within 450 mA" "curl -s $R/sim/leds | python3 -c 'import sys,json;l=json.load(sys.stdin)[\"leds\"];sys.exit(0 if len(l)==43 and any(sum(c) for c in l) else 1)' && python3 -c 'import sys;sys.exit(0 if $(st 'd["current_ma"]') <= 450.001 else 1)'"

echo "== v0.33: the budget ceiling and the channel scales, through the usermod's own settings path =="
ledsum() { curl -s $R/sim/leds | python3 -c 'import sys,json;print(sum(sum(c) for c in json.load(sys.stdin)["leds"]))'; }
curl -s -X POST $R/sim/strip -d '{"budget_ma":5000,"ma_per_led":200}' >/dev/null; sleep 0.4
MA=$(st 'd["current_ma"]')
check "budgetMa 5000 is cut to the build's ceiling: 450 < $MA <= 900 mA" "python3 -c 'import sys;sys.exit(0 if 450 < $MA <= 900.001 else 1)'"
curl -s -X POST $R/sim/strip -d '{"budget_ma":450,"ma_per_led":200}' >/dev/null; sleep 0.4
S1SUM=$(ledsum)
curl -s -X POST $R/sim/strip -d '{"ma_scale_r":2.5,"ma_scale_g":2.5,"ma_scale_b":2.5}' >/dev/null; sleep 0.4
S2SUM=$(ledsum); MA=$(st 'd["current_ma"]')
check "channel scales 2.5: the strip is dimmer ($S1SUM -> $S2SUM) and still within 450 mA ($MA)" "[ $S2SUM -lt $S1SUM ] && [ $S2SUM -gt 0 ] && python3 -c 'import sys;sys.exit(0 if $MA <= 450.001 else 1)'"
curl -s -X POST $R/sim/strip -d '{"budget_ma":450,"ma_per_led":60,"ma_scale_r":1,"ma_scale_g":1,"ma_scale_b":1}' >/dev/null

echo "== a new handshake while the session is open (the TV after a reconnect) =="
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$U" "$K" $SP 3 >/dev/null & S2=$!
sleep 2
check "old session ended at once, new handshake within 3 s, still live" "grep -q 'ending this session now\|started a new handshake' $LOG && [ \$(lock) = none ] && [ \$(st 'd[\"live\"]') = True ] && python3 -c 'import sys;sys.exit(0 if 0 <= $(st 'd["handshake_latency_s"]') < 3 else 1)'"
kill $S1 2>/dev/null; wait $S2 2>/dev/null

echo "== the stream stops: hold, fade, black =="
sleep 0.6; check "0.6 s after the last frame: held (not black)" "curl -s $R/sim/leds | python3 -c 'import sys,json;l=json.load(sys.stdin)[\"leds\"];sys.exit(0 if any(sum(c) for c in l) else 1)'"
sleep 2.2; check "2.8 s after: black, still ours (not released)" "curl -s $R/sim/leds | python3 -c 'import sys,json;d=json.load(sys.stdin);sys.exit(0 if not any(sum(c) for c in d[\"leds\"]) and not d[\"released\"] else 1)' && [ \$(st 'd[\"phase\"]') = black ]"

echo "== stream.active=true and no handshake / a wrong key =="
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null; sleep 5.5
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
sleep 2; check "not yet at 2 s" "[ \$(lock) = none ]"
sleep 1.6; check "after 3 s: locked (timeout), logged" "[ \$(lock) = timeout ] && grep -q 'locked itself' $LOG"
curl -s -X POST $R/api/$U/groups -d '{"lights":["1","2","3"],"type":"LightGroup"}' >/dev/null
check "a LightGroup afterwards confirms it" "[ \$(lock) = timeout+confirmed ]"
"$T/sender" "$U" "$K" $SP 2 >/dev/null; sleep 0.3
check "a good handshake clears the lock" "[ \$(lock) = none ]"
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null; sleep 0.5
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
timeout 5 "$T/sender" "$U" "00112233445566778899AABBCCDDEEFF" $SP 1 >/dev/null 2>&1; sleep 2
check "wrong key: locked (failed)" "[ \$(lock) = failed ]"

echo "== reboot: pairing and groups come back from NVS =="
kill -9 $SIMPID 2>/dev/null; sleep 0.5; start
check "the paired user is still known (no error 1)" "curl -s $R/api/$U/lights/1 | grep -q '\"state\"'"
check "groups 1 and 3 are there, 2 is still a placeholder" "curl -s $R/api/$U/groups/1 | grep -q Entertainment && curl -s $R/api/$U/groups/3 | grep -q '\"lights\":\[\"1\"\]' && ! curl -s $R/api/$U/groups/2 | grep -q error"
check "the lock does not survive a reboot (state, not stored)" "[ \$(lock) = none ]"
"$T/sender" "$U" "$K" $SP 2 >/dev/null & S3=$!
sleep 1.2
check "the stored clientkey still works for DTLS" "[ \$(st 'd[\"live\"]') = True ]"
wait $S3
check "pairing is closed again after the reboot" "curl -s -X POST $R/api -d '{\"devicetype\":\"other#1\"}' | grep -q '\"type\":101'"
check "nothing secret in the log (no clientkey, no full username)" "! grep -qi \"$K\" $LOG && ! grep -q \"$U\" $LOG"

kill -9 $SIMPID 2>/dev/null
echo; echo "$OKS [ok], $FAILS failed"; [ $FAILS = 0 ] && echo PASS || echo "FAILED (log: $LOG)"
exit $FAILS
