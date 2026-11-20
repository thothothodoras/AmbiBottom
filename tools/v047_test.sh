#!/bin/bash
# v0.47: the JointSpace memory guard (v0.46 on the device: heap_min 656 B and panics with
# DTLS + JointSpace + the page). The simulator's fake heap (/sim/heap?free=&block=) stands in
# for the ESP's. Loopback only, test ports, tools/mock_tv.py as the TV.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18499; SPORT=21909; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
st() { curl -s -m 3 $H/ambi/api/state; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
jss() { st | js 'd["sources"]["jointspace"]["status"],d["sources"]["jointspace"].get("error") or ""'; }
heap() { curl -s "$H/sim/heap?free=$1&block=$2" > /dev/null; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
(cd "$T" && exec python3 mock_tv.py 1926 > "$D/mock.log" 2>&1) & MOCK=$!
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8
curl -s -XPOST -d '{"username":"tvuser","password":"tvpass","tv_ip":"127.0.0.1"}' $H/ambi/api/settings/jointspace > /dev/null
curl -s -XPOST -d '{"action":"start"}' $H/ambi/api/service > /dev/null
curl -s -XPOST $H/sim/press > /dev/null
RESP=$(curl -s -XPOST $H/api -d '{"devicetype":"v047#test","generateclientkey":true}')
U=$(echo "$RESP" | js 'd[0]["success"]["username"]'); K=$(echo "$RESP" | js 'd[0]["success"]["clientkey"]')
G=$(curl -s -XPOST $H/api/$U/groups -d '{"lights":["1","2","3"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
heap 60000 30000   # too little to open a TLS connection
SENDER_HOST=127.0.0.1 SENDER_LAMPS=3 "$T/sender" "$U" "$K" $SPORT 16 > "$D/sender.log" 2>&1 & SP=$!
sleep 1.5
check "stream just started: JointSpace waits (no connection in the first seconds)" "[ \"\$(grep '^{\"connections' $D/mock.log | tail -1 | js 'd[\"requests\"]')\" = 0 ]"
sleep 4
check "low heap (60 KB free, 30 KB block): JointSpace does not connect, says low_memory" "[ \"\$(jss)\" = 'unavailable low_memory' ] && [ \"\$(grep '^{\"connections' $D/mock.log | tail -1 | js 'd[\"requests\"]')\" = 0 ]"
heap 200000 100000
sleep 2.5
check "enough heap again: JointSpace live" "[ \"\$(st | js 'd[\"sources\"][\"jointspace\"][\"status\"]')\" = live ]"
heap 15000 10000   # the heap runs out while connected
sleep 1.5
check "heap runs out while connected: the connection is closed (low_memory), counted in diag" "[ \"\$(jss)\" = 'unavailable low_memory' ] && [ \"\$(curl -s $H/ambi/api/diag | js 'd[\"app\"][\"js_lowmem\"]>=1')\" = True ]"
heap 200000 100000
sleep 2
check "after a drop it pauses (no reconnect within 30 s)" "[ \"\$(jss)\" = 'unavailable low_memory' ]"
check "the strip kept streaming meanwhile (fps > 15)" "[ \"\$(curl -s $H/ambi/api/diag | js 'd[\"app\"][\"fps\"]>15')\" = True ]"
kill $SP 2>/dev/null; wait $SP 2>/dev/null
kill $SIMPID $MOCK 2>/dev/null; wait $SIMPID $MOCK 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
