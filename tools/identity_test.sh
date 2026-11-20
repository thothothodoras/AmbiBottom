#!/bin/bash
# v0.17 end-to-end test, no TV needed: the identity card's API.
#  1) fresh state dir: the old default identity (00:17:88:a4:5e:01), 3 lamps
#  2) pair on it; from the LAN address the API refuses changes
#  3) create a new identity from the local page (random 00:17:88 MAC),
#     rename it, give it 5 lamps -> restart_needed, nothing changes yet
#  4) restart: description.xml / config carry the new MAC + name, 5 lights,
#     the old pairing is NOT valid there (its own pairing.json), and the
#     default's pairing.json is untouched
#  5) select the default again + restart: the old pairing works again
# The User-Agent log lines (v0.17) are checked on the way.
#
# usage: tools/identity_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains
# "state-dir /tmp/e2e_id" (the pkill below would kill that shell).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
LAN=$(hostname -I | awk '{print $1}')
M=18120; R=18095; S=/tmp/e2e_id
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
start() {
  "$BIN" --state-dir "$S" --port $R --stream-port 12120 --monitor-port $M --console quiet --pairing auto >> "$S.log" 2>&1 &
  BP=$!; sleep 1.5
}
stop() { kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null; }
post() { curl -s -X POST http://127.0.0.1:$M/api/identity -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' -d "$1"; }

pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S" "$S.log"
start
echo "== 1) default identity =="
C=$(curl -s http://127.0.0.1:$R/api/config | json 'd["mac"], d["name"]')
check "config: 00:17:88:a4:5e:01 FakeBridge-A45E01" "$([ "$C" = "00:17:88:a4:5e:01 FakeBridge-A45E01" ]; echo $?)"
I=$(curl -s http://127.0.0.1:$M/api/identity | json 'len(d["identities"]), d["running"]["lamps"], d["restart_needed"], d["can_edit"]')
check "identity API: 1 identity, 3 lamps, no restart needed, editable locally" "$([ "$I" = "1 3 False True" ]; echo $?)"
USER0=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#id","generateclientkey":true}' | json 'd[0]["success"]["username"]')
check "paired on the default identity" "$([ ${#USER0} = 32 ]; echo $?)"
curl -s -A "e2e-agent/1.0 (test)" http://127.0.0.1:$R/description.xml >/dev/null
check "log names who asked for description.xml" "$(grep -q 'description.xml asked by User-Agent: e2e-agent/1.0 (test)' "$S.log"; echo $?)"

echo "== 2) LAN refused =="
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://$LAN:$M/api/identity -H "Origin: http://$LAN:$M" -d '{"action":"create"}')
check "create from LAN -> 403" "$([ "$C" = 403 ]; echo $?)"
E=$(curl -s http://$LAN:$M/api/identity | json 'd["can_edit"]')
check "GET from LAN: can_edit=false" "$([ "$E" = False ]; echo $?)"

echo "== 3) new identity from the local page =="
NEW=$(post '{"action":"create"}' | json 'd["active"]')
check "created a random 00:17:88 MAC ($NEW)" "$([[ "$NEW" =~ ^00:17:88:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}$ ]] && [ "$NEW" != 00:17:88:a4:5e:01 ]; echo $?)"
post "{\"action\":\"rename\",\"mac\":\"$NEW\",\"name\":\"E2E Bridge\"}" >/dev/null
P=$(post "{\"action\":\"lamps\",\"mac\":\"$NEW\",\"lamps\":5}" | json 'd["restart_needed"], [e["name"] for e in d["identities"] if e["active"]][0], [e["lamps"] for e in d["identities"] if e["active"]][0]')
check "renamed + 5 lamps, restart_needed" "$([ "$P" = "True E2E Bridge 5" ]; echo $?)"
B=$(post "{\"action\":\"rename\",\"mac\":\"$NEW\",\"name\":\"Σαλόνι\"}" | json 'd.get("error")')
check "non-ASCII name refused" "$([ "$B" = name_not_ascii ]; echo $?)"
C=$(curl -s http://127.0.0.1:$R/api/config | json 'd["mac"]')
check "still running the default until restart" "$([ "$C" = 00:17:88:a4:5e:01 ]; echo $?)"

echo "== 4) restart with the new identity =="
stop; start
C=$(curl -s http://127.0.0.1:$R/api/config | json 'd["mac"], d["name"]')
check "config: $NEW E2E Bridge" "$([ "$C" = "$NEW E2E Bridge" ]; echo $?)"
X=$(curl -s http://127.0.0.1:$R/description.xml)
check "description.xml: new name + serial" "$(echo "$X" | grep -q '<friendlyName>E2E Bridge</friendlyName>' && echo "$X" | grep -q "<serialNumber>${NEW//:/}</serialNumber>"; echo $?)"
# (The REST API answers any username, like the Python bridge and on purpose;
# what counts is the DTLS key, looked up in the identity's own pairing.json.)
check "old user (and its DTLS key) not in the new identity's pairing.json" \
  "$(! grep -q "$USER0" "$S/identities/${NEW//:/}/pairing.json" 2>/dev/null; echo $?)"
USER1=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#id2","generateclientkey":true}' | json 'd[0]["success"]["username"]')
N=$(curl -s http://127.0.0.1:$R/api/$USER1/lights | json 'len(d)')
check "new pairing works, 5 lights" "$([ "$N" = 5 ]; echo $?)"
check "its own pairing.json in identities/, with the new user" "$(grep -q "$USER1" "$S/identities/${NEW//:/}/pairing.json"; echo $?)"
check "default pairing.json untouched (still has the first user only)" \
  "$([ "$(json 'len(d["users"]) if "users" in d else len(d)' < "$S/pairing.json")" = 1 ]; echo $?)"
I=$(curl -s http://127.0.0.1:$M/api/identity | json 'd["restart_needed"], d["running"]["mac"]==d["active"]')
check "identity API: running = active, no restart needed" "$([ "$I" = "False True" ]; echo $?)"

echo "== 5) back to the default =="
post '{"action":"select","mac":"00:17:88:a4:5e:01"}' >/dev/null
stop; start
C=$(curl -s http://127.0.0.1:$R/api/config | json 'd["mac"]')
N=$(curl -s http://127.0.0.1:$R/api/$USER0/lights | json 'len(d)')
check "default again: old MAC, 3 lights" "$([ "$C $N" = "00:17:88:a4:5e:01 3" ]; echo $?)"
check "default pairing.json still has the old user, not the new one" \
  "$(grep -q "$USER0" "$S/pairing.json" && ! grep -q "$USER1" "$S/pairing.json"; echo $?)"
stop
echo
[ $RC = 0 ] && echo "identity_test: PASS" || echo "identity_test: FAIL"
exit $RC
