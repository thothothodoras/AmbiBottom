#!/bin/bash
# v0.31 end to end, no TV: what the TV checks before it asks for the stream,
# group ids, and the DTLS timing / "TV locked to LightGroup" indicator.
# Expect 23 [ok] and PASS.
# Don't launch it from a shell whose own command line contains "state-dir /tmp/e2e_v31".
#
# usage: tools/v031_test.sh [bridge binary]
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
R=http://127.0.0.1:18331; M=http://127.0.0.1:18339; SP=12331; LOG=/tmp/e2e_v31.log
FAILS=0; OKS=0
ok() { OKS=$((OKS+1)); echo "[ok] $1"; }
bad() { FAILS=$((FAILS+1)); echo "[FAIL] $1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
lock() { curl -s $M/api/state | python3 -c 'import sys,json;l=json.load(sys.stdin)["sources"]["hue"].get("tv_dtls_lock");print("none" if not l else l["reason"]+("+confirmed" if l["confirmed"] else ""))'; }
pkill -9 -f "state-dir /tmp/e2e_v31" 2>/dev/null; sleep 0.5; rm -rf /tmp/e2e_v31; mkdir -p /tmp/e2e_v31
# an older store: ids from 2, copies of the same group (what v0.30 and before left behind)
cat > /tmp/e2e_v31/groups.json <<'JSON'
{"2":{"name":"TV","type":"Entertainment","class":"TV","lights":["1","2","3"],"locations":{}},
 "3":{"name":"Group 3","type":"LightGroup","lights":["1","2","3"],"locations":{}},
 "4":{"name":"TV","type":"Entertainment","class":"TV","lights":["1","2","3"],"locations":{}},
 "5":{"name":"Group 5","type":"LightGroup","lights":["1","2","3"],"locations":{}},
 "6":{"name":"TV","type":"Entertainment","class":"TV","lights":["1","2","3"],"locations":{}}}
JSON
"$BIN" --state-dir /tmp/e2e_v31 --port 18331 --stream-port $SP --monitor-port 18339 --console quiet --pairing auto > $LOG 2>&1 &
BP=$!; sleep 1.5
RESP=$(curl -s -X POST $R/api -d '{"devicetype":"v031#test","generateclientkey":true}')
U=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])')
K=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])')

echo "== version, config, capabilities =="
check "version v0.3x" "grep -Eq 'v0\.3[1-9]' $LOG"
check "/config modelid BSB002 and apiversion >= 1.22" "curl -s $R/api/$U/config | python3 -c 'import sys,json;c=json.load(sys.stdin);a=c[\"apiversion\"].split(\".\");sys.exit(0 if c[\"modelid\"]==\"BSB002\" and (int(a[0])>1 or int(a[1])>=22) else 1)'"
check "/capabilities streaming.available > 0 (and groups present)" "curl -s $R/api/$U/capabilities | python3 -c 'import sys,json;c=json.load(sys.stdin);sys.exit(0 if c[\"streaming\"][\"available\"]>0 and \"groups\" in c else 1)'"

echo "== groups =="
check "old store tidied: 5 stored -> 2 groups, ids 1..2, log says so" "grep -q 'groups store tidied: 5 stored group(s) -> 2' $LOG"
check "group 1 is the Entertainment group" "curl -s $R/api/$U/groups/1 | grep -q '\"type\":\"Entertainment\"'"
check "old id 6 (the TV may remember it): no error" "! curl -s $R/api/$U/groups/6 | grep -q error"
check "id 7 never existed: error (ends the TV's search)" "curl -s $R/api/$U/groups/7 | grep -q '\"error\"'"
check "POST same type+lights -> existing id 1" "curl -s -X POST $R/api/$U/groups -d '{\"lights\":[\"1\",\"2\",\"3\"],\"type\":\"Entertainment\",\"class\":\"TV\"}' | grep -q '\"id\":\"1\"'"
check "DELETE /groups/2 removes it from the list and from groups.json" "curl -s -X DELETE $R/api/$U/groups/2 | grep -q deleted && ! curl -s $R/api/$U/groups | grep -q '\"2\":' && python3 -c 'import json,sys;sys.exit(0 if \"2\" not in json.load(open(\"/tmp/e2e_v31/groups.json\")) else 1)'"

check "the log never has the full username (cut to 8 characters since v0.32)" "! grep -q \"$U\" $LOG && grep -q \"/api/\${U:0:8}\\.\\.\\./\" $LOG"

echo "== 10x GET /lights/1 with two silent connections open (TV limit 300 ms each) =="
exec 7<>/dev/tcp/127.0.0.1/18331; exec 8<>/dev/tcp/127.0.0.1/18331
WORST=0; ALL200=1
for i in 1 2 3 4 5 6 7 8 9 10; do
  O=$(curl -s -o /dev/null -w '%{http_code} %{time_total}' --max-time 2 $R/api/$U/lights/1)
  [ "${O%% *}" = 200 ] || ALL200=0
  WORST=$(python3 -c "print(max($WORST, ${O##* }))")
done
exec 7>&-; exec 8>&-
check "all ten answered 200, worst ${WORST}s < 0.300" "[ $ALL200 = 1 ] && python3 -c 'import sys;sys.exit(0 if $WORST < 0.3 else 1)'"
check "/lights/9 (no such lamp) still HTTP 200" "[ \$(curl -s -o /dev/null -w '%{http_code}' $R/api/$U/lights/9) = 200 ]"

echo "== unknown 32-hex username =="
curl -s $R/api/0123456789abcdef0123456789abcdef/lights >/dev/null; curl -s $R/api/0123456789abcdef0123456789abcdef/groups/1 >/dev/null
check "logged once: the TV has LOST ITS PAIRING" "[ \$(grep -c 'LOST ITS PAIRING' $LOG) = 1 ]"

echo "== the TV's order: active=false, active=true, handshake =="
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$U" "$K" $SP 8 >/dev/null & S1=$!
sleep 2
check "handshake time after stream.active=true is logged and under 3 s" "grep -q 'DTLS handshake 0\.[0-9]* s after stream.active=true' $LOG"
check "no lock while streaming" "[ \$(lock) = none ]"

echo "== a second handshake while the first session is still open (the TV after a reconnect) =="
N0=$(grep -c 'DTLS handshake .* after stream.active=true' $LOG)
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$U" "$K" $SP 3 >/dev/null & S2=$!
sleep 2.5
check "old session ended at once (not after the 5 s idle timeout)" "grep -q 'stream.active changed: ending this session now' $LOG"
check "the new handshake came within 3 s" "[ \$(grep -c 'DTLS handshake [0-2]\.[0-9]* s after stream.active=true' $LOG) -gt $N0 ] && [ \$(lock) = none ]"
kill $S1 2>/dev/null; wait $S2 2>/dev/null; sleep 0.5

echo "== stream.active=true and no handshake: the TV would lock itself =="
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null
sleep 6
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
sleep 2; check "not yet at 2 s" "[ \$(lock) = none ]"
sleep 2; check "after 3 s: lock (timeout) on the monitor and in the log" "[ \$(lock) = timeout ] && grep -q 'locked' $LOG"
curl -s -X POST $R/api/$U/groups -d '{"lights":["1","2","3"],"type":"LightGroup"}' >/dev/null
check "the TV then creating a LightGroup confirms it" "[ \$(lock) = timeout+confirmed ]"
check "page has the banner text" "curl -s $M/ | grep -q 'Η TV κλείδωσε σε LightGroup μετά από αποτυχία DTLS'"
"$T/sender" "$U" "$K" $SP 2 >/dev/null; sleep 0.3
check "a good handshake clears the lock" "[ \$(lock) = none ]"

echo "== stream.active=true and a handshake with the wrong key =="
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null; sleep 0.5
curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
timeout 5 "$T/sender" "$U" "00112233445566778899AABBCCDDEEFF" $SP 1 >/dev/null 2>&1
sleep 0.5
check "lock (failed)" "[ \$(lock) = failed ]"

kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null
echo; echo "$OKS [ok], $FAILS failed"; [ $FAILS = 0 ] && echo PASS || echo "FAILED (log: $LOG)"
exit $FAILS
