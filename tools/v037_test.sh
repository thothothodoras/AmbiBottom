#!/bin/bash
# v0.37: the AmbiBottom page (/ambi) and its API, against the simulator (build/esp_app_sim =
# the firmware's App code on Linux). Loopback only, test ports, nothing on the LAN.
# Checks: the generated page is up to date and only calls /ambi/api; boots stopped; Start /
# Stop; Demo (bri / fx limits, shown while there is no stream, the TV's stream wins over it,
# back to the demo when the stream ends); strip settings (partial merge, led_count = WLED's,
# kept across a restart); Linux-only paths answer 404; the page's scripts parse.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18491; SPORT=21901; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
st() { curl -s -m 3 $H/ambi/api/state; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
start_sim() { "$SIM" "$D/state" $HP $SPORT 5 >> "$D/sim.log" 2>&1 & SIMPID=$!; sleep 0.8; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1

python3 "$T/make_esp_page.py" --check > /dev/null && ok "generated page matches src/monitor_server.cpp" || bad "page out of date (tools/make_esp_page.py)"
P="$R/firmware/wled-usermod/AmbiBottom/ambi_page.html"
check "page calls no /api/ path directly (only /ambi/api/)" "! grep -q \"fetch('/api/\" '$P'"
check "page has the ESP panel and no EventSource use" "grep -q 'id=\"espPanel\"' '$P' && grep -q 'if(false&&window.EventSource)' '$P'"
python3 - "$P" "$D" <<'EOF'
import re, sys
p = open(sys.argv[1], encoding="utf-8").read()
for i, s in enumerate(re.findall(r"<script>(.*?)</script>", p, re.S)):
    open(f"{sys.argv[2]}/s{i}.js", "w", encoding="utf-8").write(s)
EOF
JSOK=1; for f in "$D"/s*.js; do node --check "$f" 2>> "$D/node.log" || JSOK=0; done
check "page scripts parse (node --check)" "[ $JSOK = 1 ]"

start_sim
check "page served at /ambi" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' $H/ambi)\" = 200 ]"
check "boots stopped" "[ \"\$(st | js 'd[\"service\"]')\" = stopped ]"
check "state has the monitor's schema" "[ \"\$(st | js 'd[\"schema\"]')\" = ambilight-monitor/1 ]"
check "Linux-only path -> 404" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' $H/ambi/api/identity)\" = 404 ]"
curl -s -XPOST -d '{"on":true,"bri_pct":0,"speed":999,"fx":3}' $H/ambi/api/demo > "$D/demo1.json"
check "demo limits: bri 0 -> 1, speed 999 -> 255" "[ \"\$(js 'd[\"bri_pct\"],d[\"speed\"],d[\"fx\"]' < $D/demo1.json)\" = '1 255 3' ]"
check "demo on but stopped: not shown" "[ \"\$(st | js 'd[\"demo\"],d[\"esp_demo\"][\"showing\"]')\" = 'False False' ]"
curl -s -XPOST -d '{"bri_pct":25,"fx":-1,"speed":60}' $H/ambi/api/demo > /dev/null
curl -s -XPOST -d '{"action":"start"}' $H/ambi/api/service > /dev/null
check "Start -> running" "[ \"\$(st | js 'd[\"service\"]')\" = running ]"
check "running + demo, no stream: demo shown, 43 LEDs" "[ \"\$(st | js 'd[\"demo\"],d[\"sources\"][\"hue\"][\"status\"],len(d[\"output\"][\"leds\"])')\" = 'True demo 43' ]"
check "bad service action -> 400" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -XPOST -d '{\"action\":\"x\"}' $H/ambi/api/service)\" = 400 ]"

# the TV's stream wins over the demo
curl -s -XPOST $H/sim/press > /dev/null
RESP=$(curl -s -XPOST $H/api -d '{"devicetype":"v037#test","generateclientkey":true}')
U=$(echo "$RESP" | js 'd[0]["success"]["username"]'); K=$(echo "$RESP" | js 'd[0]["success"]["clientkey"]')
G=$(curl -s -XPOST $H/api/$U/groups -d '{"lights":["1","2","3","4","5"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=5 "$T/sender" "$U" "$K" $SPORT 6 > "$D/sender.log" 2>&1 &
SP=$!; sleep 3
check "stream live: TV shown, not the demo; 5 lamps with colours" "[ \"\$(st | js 'd[\"demo\"],d[\"sources\"][\"hue\"][\"status\"],sum(1 for b in d[\"bottom\"] if b[\"rgb\"])')\" = 'False live 5' ]"
check "stream live: strip is the pipeline's (limit info present)" "[ \"\$(st | js 'd[\"output\"][\"available\"] and d[\"output\"][\"limit\"][\"mode\"] in (\"none\",\"peaks\",\"anchors\",\"uniform\")')\" = True ]"
wait $SP
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":false}}' > /dev/null
sleep 8
check "after the stream: the demo switched itself off (v0.43: the TV was seen)" "[ \"\$(st | js 'd[\"demo\"],d[\"esp_demo\"][\"on\"]')\" = 'False False' ]"
curl -s -XPOST -d '{"on":false}' $H/ambi/api/demo > /dev/null
check "demo off while running: no demo, strip dark (black phase or nothing)" "[ \"\$(st | js 'd[\"demo\"],not any(sum(c) for c in (d[\"output\"][\"leds\"] or []))')\" = 'False True' ]"
curl -s -XPOST -d '{"action":"stop"}' $H/ambi/api/service > /dev/null
check "Stop -> stopped" "[ \"\$(st | js 'd[\"service\"]')\" = stopped ]"

# v0.37: the ESP restarts while the TV streams (OTA, power): the TV keeps its old DTLS session.
# The lamp check (GET lights/1) then fails with 503 so the TV goes to its Reconnection state.
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=5 "$T/sender" "$U" "$K" $SPORT 12 > "$D/sender_old.log" 2>&1 & SP=$!
sleep 2
kill -9 $SIMPID; wait $SIMPID 2>/dev/null; start_sim
sleep 3.5
check "old session: lamp check GET lights/1 -> 503" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' $H/api/$U/lights/1)\" = 503 ]"
check "old session: other requests unchanged (GET lights -> 200)" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' $H/api/$U/lights)\" = 200 ]"
check "old session: shown on the page (hue.old_session)" "[ \"\$(st | js 'd[\"sources\"][\"hue\"][\"old_session\"][\"ip\"]')\" = 127.0.0.1 ]"
wait $SP; sleep 4
check "old session over: lamp check 200 again" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' $H/api/$U/lights/1)\" = 200 ]"

# strip settings: partial merge, led_count stays WLED's, kept across a restart
curl -s -XPOST -d '{"current_budget_ma":330,"led_count":10,"rise_ms":200,"blend_mode":"zones"}' $H/ambi/api/settings/strip > "$D/set.json"
check "settings: budget 330, led_count 10 (LEDs used), saved" "[ \"\$(js 'd[\"settings\"][\"current_budget_ma\"],d[\"settings\"][\"led_count\"],d[\"settings\"][\"rise_ms\"],d[\"settings\"][\"blend_mode\"],d[\"saved_to_disk\"]' < $D/set.json)\" = '330 10 200 zones True' ]"
check "settings: led_count above WLED's Length (43) clamped to it" "[ \"\$(curl -s -XPOST -d '{\"led_count\":100}' $H/ambi/api/settings/strip | js 'd[\"settings\"][\"led_count\"],d[\"limits\"][\"led_count_max\"]')\" = '43 43' ]"
check "settings: budget above the ceiling clamped to 900" "[ \"\$(curl -s -XPOST -d '{\"current_budget_ma\":5000}' $H/ambi/api/settings/strip | js 'd[\"settings\"][\"current_budget_ma\"]')\" = 900 ]"
curl -s -XPOST -d '{"current_budget_ma":330}' $H/ambi/api/settings/strip > /dev/null
check "settings: not JSON -> 400" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -XPOST -d 'nope' $H/ambi/api/settings/strip)\" = 400 ]"
kill $SIMPID; wait $SIMPID 2>/dev/null; start_sim
# (the simulator applies its own StripConfig defaults at start, as the usermod applies its cfg.json
#  fields: the fields the usermod exposes come from there, the others from the saved settings)
check "after restart: page-only settings kept (blend zones, rise from cfg)" "[ \"\$(curl -s $H/ambi/api/settings/strip | js 'd[\"settings\"][\"blend_mode\"]')\" = zones ]"
check "after restart: stopped again" "[ \"\$(st | js 'd[\"service\"]')\" = stopped ]"
kill $SIMPID; wait $SIMPID 2>/dev/null
grep -q "$K" "$D/sim.log" && bad "clientkey in the log" || ok "no clientkey in the log"

# v0.37: JointSpace on the ESP (the shared client) against tools/mock_tv.py on 1926 (tvuser / tvpass)
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
(cd "$T" && exec python3 mock_tv.py 1926 > "$D/mock.log" 2>&1) & MOCK=$!
rm -rf "$D/state"; mkdir -p "$D/state"; start_sim
jss() { st | js 'd["sources"]["jointspace"]["status"]'; }
check "JointSpace: not configured at first" "[ \"\$(jss)\" = not_configured ]"
check "JointSpace: half credentials -> 422" "[ \"\$(curl -s -o /dev/null -w '%{http_code}' -XPOST -d '{\"username\":\"tvuser\"}' $H/ambi/api/settings/jointspace)\" = 422 ]"
curl -s -XPOST -d '{"username":"tvuser","password":"tvpass"}' $H/ambi/api/settings/jointspace > "$D/jsset.json"
curl -s $H/ambi/api/settings/jointspace > "$D/jsget.json"
check "JointSpace: saved, GET never shows them" "grep -q '\"configured\":true' $D/jsset.json && ! grep -q tvpass $D/jsget.json && ! grep -q tvuser $D/jsget.json"
sleep 1.5
check "JointSpace: no TV stream -> TV not asked (inactive)" "[ \"\$(jss)\" = inactive ]"
pair_stream() {  # $1 = seconds
  curl -s -XPOST $H/sim/press > /dev/null
  local R; R=$(curl -s -XPOST $H/api -d '{"devicetype":"v037js#test","generateclientkey":true}')
  U=$(echo "$R" | js 'd[0]["success"]["username"]'); K=$(echo "$R" | js 'd[0]["success"]["clientkey"]')
  G=$(curl -s -XPOST $H/api/$U/groups -d '{"lights":["1","2","3","4","5"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
  curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
  SENDER_HOST=127.0.0.1 SENDER_LAMPS=5 "$T/sender" "$U" "$K" $SPORT "$1" > "$D/sender_js.log" 2>&1 & SP=$!
}
pair_stream 11; sleep 6.5  # v0.47: JointSpace waits 4 s after the stream starts
check "JointSpace: live while the TV streams, zones 3/8/3" "[ \"\$(st | js 'd[\"sources\"][\"jointspace\"][\"status\"],d[\"topology\"][\"left\"],d[\"topology\"][\"top\"],d[\"topology\"][\"right\"]')\" = 'live 3 8 3' ]"
check "JointSpace: TV address taken from the Hue stream" "curl -s $H/ambi/api/settings/jointspace | grep -q '\"tv_ip_from\":\"hue_stream\"'"
curl -s -XPOST -d '{"edge_source":"jointspace"}' $H/ambi/api/settings/strip > /dev/null; sleep 1.5
check "JointSpace: side colours reach the strip (edge_source jointspace, corner weight > 0)" "[ \"\$(st | js 'd[\"output\"][\"limit\"][\"edge_source\"], d[\"output\"][\"limit\"][\"corner_weight\"] > 0')\" = 'jointspace True' ]"
wait $SP
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":false}}' > /dev/null
sleep 11
check "JointSpace: TV stopped streaming -> no more requests (inactive)" "[ \"\$(jss)\" = inactive ]"
check "JointSpace: kept-alive connection, no bad auth (mock TV counters)" "tail -1 $D/mock.log | python3 -c 'import json,sys;d=json.loads(sys.stdin.read());sys.exit(0 if d[\"connections\"]<=2 and d[\"challenges\"]<=2 and d[\"bad_auth\"]==0 and d[\"ok\"]>5 else 1)'"
curl -s -XPOST -d '{"username":"tvuser","password":"wrong"}' $H/ambi/api/settings/jointspace > /dev/null
pair_stream 8; sleep 6.5
check "JointSpace: wrong password -> authentication_failed" "[ \"\$(st | js 'd[\"sources\"][\"jointspace\"][\"error\"]')\" = authentication_failed ]"
wait $SP
kill $SIMPID $MOCK 2>/dev/null; wait $SIMPID $MOCK 2>/dev/null
grep -q tvpass "$D/sim.log" && bad "JointSpace password in the log" || ok "no JointSpace password in the log"
echo; echo "$OK [ok], $BAD failed"; rm -rf "$D"
[ $BAD = 0 ] && echo PASS || { echo FAIL; exit 1; }
