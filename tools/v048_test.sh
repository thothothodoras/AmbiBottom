#!/bin/bash
# v0.48: the PIN dialog outside the tabs, JointSpace on / off, white balance, the English-only
# switch of the page - against the simulator (build/esp_app_sim) and tools/mock_tv.py.
# Loopback only, test ports. (The black box and the page's heap floor live in the usermod:
# checked on the device.)
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18500; SPORT=21910; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
st() { curl -s -m 3 $H/ambi/api/state; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
mreq() { sleep 1.1; grep '^{"connections' "$D/mock.log" | tail -1 | js 'd["requests"]'; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
(cd "$T" && exec python3 mock_tv.py 1926 > "$D/mock.log" 2>&1) & MOCK=$!
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8

# ---- page
P="$R/firmware/wled-usermod/AmbiBottom/ambi_page.html"
pin_ok() { python3 - "$1" <<'EOF'
import re, sys
p = open(sys.argv[1], encoding="utf-8").read()
b = re.search(r"<body[^>]*>", p).end()
d = p.index('<dialog id="pinDialog"')
# right under <body>, before (outside) the tab panes
sys.exit(0 if b <= d < p.index('id="paneLive"') and d < p.index('id="paneBridge"') else 1)
EOF
}
check "page: the PIN dialog sits under <body>, outside both tabs (opens from Settings too)" "pin_ok $P && pin_ok ${P%.html}_el.html"
check "page: JointSpace on / off switch, white balance R / G / B fields" "grep -q 'id=\"jsOn\"' $P && grep -q 'id=\"setWbR\"' $P && grep -q 'id=\"setWbG\"' $P && grep -q 'id=\"setWbB\"' $P"
check "page: the language button hides itself on an English-only build (page_el false)" "grep -q \"dev.page_el===false\" $P"
check "ambi_page_gz.h: the Greek copy is behind AMBIBOTTOM_PAGE_EL" "grep -q '^#if AMBIBOTTOM_PAGE_EL' $R/firmware/wled-usermod/AmbiBottom/ambi_page_gz.h"

# ---- white balance through the settings API
curl -s -XPOST -d '{"wb_r":1,"wb_g":0.9,"wb_b":0.75}' $H/ambi/api/settings/strip > "$D/wb.json"
check "white balance saved (0.9 / 0.75) and returned" "[ \"\$(curl -s $H/ambi/api/settings/strip | js 'd[\"settings\"][\"wb_g\"],d[\"settings\"][\"wb_b\"]')\" = '0.9 0.75' ]"
check "white balance below 0.2 -> clamped or refused, never 0" "curl -s -o /dev/null -XPOST -d '{\"wb_b\":0}' $H/ambi/api/settings/strip; [ \"\$(curl -s $H/ambi/api/settings/strip | js 'd[\"settings\"][\"wb_b\"]>=0.2')\" = True ]"

# ---- JointSpace off: no connection while the TV streams
curl -s -XPOST -d '{"username":"tvuser","password":"tvpass","tv_ip":"127.0.0.1"}' $H/ambi/api/settings/jointspace > /dev/null
check "JointSpace: enabled by default (settings GET)" "[ \"\$(curl -s $H/ambi/api/settings/jointspace | js 'd[\"enabled\"]')\" = True ]"
curl -s -XPOST -d '{"enabled":false}' $H/ambi/api/settings/jointspace > /dev/null
curl -s -XPOST -d '{"action":"start"}' $H/ambi/api/service > /dev/null
curl -s -XPOST $H/sim/press > /dev/null
RESP=$(curl -s -XPOST $H/api -d '{"devicetype":"v048#test","generateclientkey":true}')
U=$(echo "$RESP" | js 'd[0]["success"]["username"]'); K=$(echo "$RESP" | js 'd[0]["success"]["clientkey"]')
G=$(curl -s -XPOST $H/api/$U/groups -d '{"lights":["1","2","3"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=3 "$T/sender" "$U" "$K" $SPORT 16 > "$D/sender.log" 2>&1 & SP=$!
sleep 6.5
check "JointSpace off: status off, no request to the TV, the strip still streams" "[ \"\$(st | js 'd[\"sources\"][\"jointspace\"][\"status\"]')\" = off ] && [ \"\$(mreq)\" = 0 ] && [ \"\$(curl -s $H/ambi/api/diag | js 'd[\"app\"][\"fps\"]>15')\" = True ]"
curl -s -XPOST -d '{"enabled":true}' $H/ambi/api/settings/jointspace > /dev/null
sleep 2.5
check "JointSpace on again: live" "[ \"\$(st | js 'd[\"sources\"][\"jointspace\"][\"status\"]')\" = live ]"
kill $SIMPID; wait $SIMPID 2>/dev/null
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8
curl -s -XPOST -d '{"enabled":false}' $H/ambi/api/settings/jointspace > /dev/null
kill $SIMPID; wait $SIMPID 2>/dev/null
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8
check "JointSpace off is kept after a restart" "[ \"\$(curl -s $H/ambi/api/settings/jointspace | js 'd[\"enabled\"]')\" = False ]"
check "the replies never carry the credentials" "! curl -s $H/ambi/api/settings/jointspace | grep -q tvpass"

kill $SP $SIMPID $MOCK 2>/dev/null; wait $SP $SIMPID $MOCK 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
