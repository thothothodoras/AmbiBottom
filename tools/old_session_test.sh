#!/bin/bash
# v0.21: the bridge is restarted while the "TV" (tools/sender) is streaming.
# (v0.29: by a crash - a clean restart keeps the session, see v029_test.sh)
# The sender keeps sending records of its old DTLS session, which the new
# bridge cannot decrypt (what the real TV did on 2026-10-31 23:25).
#  1) one clear log line + a 30 s summary instead of 25 failures a second
#  2) /api/state sources.hue.old_session = {ip, packets} -> page banner
#  3) a new handshake (Ambilight+hue off/on on the TV = a new sender)
#     clears it: "new session ... after N old-session packets", hue live
#
# usage: tools/old_session_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains
# "state-dir /tmp/e2e_old" (pkill below).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
M=18150; R=18096; D=12150; S=/tmp/e2e_old
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
start() {
  "$BIN" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet --pairing auto >> "$S.log" 2>&1 &
  BP=$!; sleep 1.5
}
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S" "$S.log"
start
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#old","generateclientkey":true}')
U=$(echo "$RESP" | json 'd[0]["success"]["username"]')
K=$(echo "$RESP" | json 'd[0]["success"]["clientkey"]')
curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$U" "$K" $D 14 >/dev/null 2>&1 &
SP=$!; sleep 2
H=$(curl -s http://127.0.0.1:$M/api/state | json 'd["sources"]["hue"]["status"]')
check "first session live ($H)" "$([ "$H" = live ]; echo $?)"

echo "== the bridge dies while the sender keeps streaming =="
# v0.29: a clean stop/restart now SAVES the session and the next run continues it
# (tools/v029_test.sh). An old session is left only after a crash / kill -9 /
# power loss, so that is what this test does now.
kill -9 $BP; sleep 0.5
check "v0.29 crash: no saved session file" "$([ ! -e "$S/dtls_session.bin" ]; echo $?)"
: > "$S.log"
start
sleep 4
curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
sleep 1
O=$(curl -s http://127.0.0.1:$M/api/state | json 'd["sources"]["hue"]["old_session"]')
check "state: hue.old_session reported ($O)" "$([[ "$O" == *"'ip': '127.0.0.1'"* ]]; echo $?)"
N=$(curl -s http://127.0.0.1:$M/api/state | json 'd["sources"]["hue"]["old_session"]["packets"]')
check "old-session packets counted ($N > 50)" "$([ "${N%.*}" -gt 50 ]; echo $?)"
check "log: one clear line with what to do" "$([ "$(grep -c 'streaming from an OLD session' "$S.log")" = 1 ]; echo $?)"
F=$(grep -c 'handshake did not complete' "$S.log")
check "log: at most 2 failure lines instead of one per packet ($F)" "$([ "$F" -le 2 ]; echo $?)"
W=$(grep -c 'waiting for a client' "$S.log")
check "log: 'waiting for a client' not repeated per packet ($W)" "$([ "$W" -le 4 ]; echo $?)"
# v0.37: the TV's lamp check fails while it streams from the old session, so the TV reconnects by itself
C1=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$R/api/$U/lights/1)
check "v0.37: lamp check GET lights/1 -> 503 during the old session ($C1)" "$([ "$C1" = 503 ]; echo $?)"
C2=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$R/api/$U/lights)
check "v0.37: other requests unchanged (GET lights -> $C2)" "$([ "$C2" = 200 ]; echo $?)"
check "v0.37: log says why the lamp check failed" "$(grep -q 'streams from an old session: lamp check -> 503' "$S.log"; echo $?)"

echo "== the TV starts a new session (Ambilight+hue off/on) =="
kill $SP 2>/dev/null; wait $SP 2>/dev/null
"$T/sender" "$U" "$K" $D 6 >/dev/null 2>&1 &
SP=$!; sleep 2.5
check "log: new session after the old one" "$(grep -q 'new session from 127.0.0.1 after [0-9]* old-session packets' "$S.log"; echo $?)"
V=$(curl -s http://127.0.0.1:$M/api/state | json 'd["sources"]["hue"]["status"], d["sources"]["hue"]["old_session"]')
check "state: hue live, old_session cleared ($V)" "$([ "$V" = "live None" ]; echo $?)"
C3=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$R/api/$U/lights/1)
check "v0.37: new session -> lamp check 200 again ($C3)" "$([ "$C3" = 200 ]; echo $?)"
wait $SP 2>/dev/null
kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null
echo
[ $RC = 0 ] && echo "old_session_test: PASS" || echo "old_session_test: FAIL"
exit $RC
