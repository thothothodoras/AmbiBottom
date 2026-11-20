#!/bin/bash
# v0.19 end-to-end: "Ποιος ρωτάει" + the "answer only Hue searches" switch.
#  1) switch off (default): a browser-like DIAL search gets our 3 answers
#  2) /api/discovery lists it with the full User-Agent; description.xml too
#  3) the switch refuses the LAN, works from the local page, is saved
#  4) switch on: DIAL search -> no answer, ssdp:all -> 3, upnp:rootdevice -> 1
#  5) restart: the switch is still on
# The searches go unicast to 127.0.0.1:1900. If another bridge also has
# :1900 open (SO_REUSEADDR), Linux hands unicast to the newest socket = this
# test's bridge; the answers are checked by the port in their LOCATION.
#
# usage: tools/discovery_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains
# "state-dir /tmp/e2e_disc" (pkill below).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
LAN=$(hostname -I | awk '{print $1}')
M=18140; R=18097; S=/tmp/e2e_disc
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
start() {
  "$BIN" --state-dir "$S" --port $R --stream-port 12140 --monitor-port $M --console quiet --pairing auto >> "$S.log" 2>&1 &
  BP=$!; sleep 1.5
}
stop() { kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null; }
# msearch <ST> -> number of answers that point at THIS bridge (LOCATION :$R)
msearch() {
  python3 - "$1" "$R" <<'EOF'
import socket, sys, time
st, port = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.3)
m = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n"
     f"ST: {st}\r\nUSER-AGENT: Mozilla/5.0 (Windows NT 10.0; Win64; x64) e2e-disc Chrome/146.0.0.0 Safari/537.36\r\n\r\n")
s.sendto(m.encode(), ("127.0.0.1", 1900))
n, end = 0, time.time() + 1.2
while time.time() < end:
    try:
        d = s.recv(2048).decode(errors="replace")
        if f":{port}/description.xml" in d: n += 1
    except socket.timeout:
        pass
print(n)
EOF
}
post() { curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:$M/api/discovery -H "Origin: http://127.0.0.1:$M" -d "$1"; }

pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S" "$S.log"
start
DIAL=urn:dial-multiscreen-org:service:dial:1
echo "== 1) switch off: every search answered =="
check "DIAL search answered 3x (as before)" "$([ "$(msearch $DIAL)" = 3 ]; echo $?)"
echo "== 2) who asks =="
curl -s -A "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36 Edg/146.0.0.0 e2e-full-ua" \
  http://127.0.0.1:$R/description.xml >/dev/null
D=$(curl -s http://127.0.0.1:$M/api/discovery)
V=$(echo "$D" | json '[(a["kind"],a["what"],a["count"],a["answered"]) for a in d["askers"] if "e2e-disc" in a["ua"]]')
check "SSDP entry: ST + answered ($V)" "$([ "$V" = "[('ssdp', '$DIAL', 1, True)]" ]; echo $?)"
V=$(echo "$D" | json '[a["ua"] for a in d["askers"] if a["kind"]=="description"][0]')
check "description.xml entry has the whole User-Agent (${#V} chars, ends ...${V: -25})" "$([[ "$V" == *"Edg/146.0.0.0 e2e-full-ua" ]]; echo $?)"
check "hue_only is off by default" "$([ "$(echo "$D" | json 'd["hue_only"]')" = False ]; echo $?)"
echo "== 3) the switch =="
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://$LAN:$M/api/discovery -H "Origin: http://$LAN:$M" -d '{"hue_only":true}')
check "switch from LAN -> 403" "$([ "$C" = 403 ]; echo $?)"
check "can_edit=false from LAN" "$([ "$(curl -s http://$LAN:$M/api/discovery | json 'd["can_edit"]')" = False ]; echo $?)"
check "bad value -> 400" "$([ "$(post '{"hue_only":"yes"}')" = 400 ]; echo $?)"
check "switch on from the local page -> 200" "$([ "$(post '{"hue_only":true}')" = 200 ]; echo $?)"
check "saved in discovery.json" "$(grep -q '"ssdp_hue_only":[[:space:]]*true' "$S/discovery.json"; echo $?)"
echo "== 4) switch on =="
check "DIAL search: no answer" "$([ "$(msearch $DIAL)" = 0 ]; echo $?)"
check "ssdp:all: 3 answers" "$([ "$(msearch ssdp:all)" = 3 ]; echo $?)"
check "upnp:rootdevice: 1 answer" "$([ "$(msearch upnp:rootdevice)" = 1 ]; echo $?)"
V=$(curl -s http://127.0.0.1:$M/api/discovery | json '[(a["count"],a["answered"]) for a in d["askers"] if a["what"]=="'$DIAL'" and "e2e-disc" in a["ua"]][0]')
check "registry: DIAL 2x, last one ignored ($V)" "$([ "$V" = "(2, False)" ]; echo $?)"
check "log says ignored" "$(grep -q "ST=$DIAL .*(not a Hue search: ignored)" "$S.log"; echo $?)"
echo "== 5) restart =="
stop; start
check "switch still on after restart" "$([ "$(curl -s http://127.0.0.1:$M/api/discovery | json 'd["hue_only"]')" = True ]; echo $?)"
check "DIAL still ignored" "$([ "$(msearch $DIAL)" = 0 ]; echo $?)"
post '{"hue_only":false}' >/dev/null
check "switch off again: DIAL answered 3x" "$([ "$(msearch $DIAL)" = 3 ]; echo $?)"
stop
echo
[ $RC = 0 ] && echo "discovery_test: PASS" || echo "discovery_test: FAIL"
exit $RC
