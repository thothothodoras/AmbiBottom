#!/bin/bash
# v0.29: the TV's DTLS session survives a stop/start, a restart and an exit + new start.
# tools/sender stands in for the TV: ONE handshake, then it keeps sending the same session for 40 s
# (like the real TV, it never notices the bridge went away).
#  A) Stop from the page while streaming -> session saved (file 600), no close_notify; Start -> the same
#     session continues: hue live again, "saved session continued", frames counted again, no OLD-session line
#  B) r r style restart (/api/restart) -> continues again
#  C) exit (Ctrl+C) + a fresh start (like `ambi restart`) -> continues again
#  D) kill -9 (crash, nothing saved) -> old-session detection as before
#  E) a corrupt / foreign saved file is not used, and is deleted
# Same pkill caution as the other tests: never run a command line containing "state-dir /tmp/e2e_v29".
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
S=/tmp/e2e_v29
R=18097; D=12137; M=18137
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
post_local() { curl -s -X POST "http://127.0.0.1:$M$1" -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' -d "$2"; }
hue() { curl -s http://127.0.0.1:$M/api/state | json 'd["sources"]["hue"]["status"]'; }
frames() { curl -s http://127.0.0.1:$M/api/state | json 'd["sources"]["hue"].get("frames",0)'; }
start() {
  "$BIN" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet --pairing auto >> "$S.log" 2>&1 &
  BP=$!; sleep 1.5
}
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S" "$S.log"
F="$S/dtls_session.bin"
start
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#v29","generateclientkey":true}')
U=$(echo "$RESP" | json 'd[0]["success"]["username"]')
K=$(echo "$RESP" | json 'd[0]["success"]["clientkey"]')
curl -s -X PUT http://127.0.0.1:$R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
"$T/sender" "$U" "$K" $D 40 >/dev/null 2>&1 &
SP=$!; sleep 2
check "first session live ($(hue))" "$([ "$(hue)" = live ]; echo $?)"

echo "== A) Stop / Start from the page while the 'TV' streams =="
post_local /api/service '{"action":"stop"}' >/dev/null; sleep 2.5
check "stop: session saved (mode 600), no close_notify" \
  "$(grep -q 'session with 127.0.0.1 saved' "$S.log" && [ "$(stat -c %a "$F")" = 600 ] && ! grep -q 'close_notify sent' "$S.log"; echo $?)"
post_local /api/service '{"action":"start"}' >/dev/null; sleep 3
check "start: the saved session continued, first frame decrypted, hue live ($(hue))" \
  "$(grep -q 'saved session continued (no new handshake)' "$S.log" && grep -q 'first frame decrypted with the saved session' "$S.log" && [ "$(hue)" = live ]; echo $?)"
G=$(curl -s http://127.0.0.1:$R/api/$U/groups/1 | json 'd["stream"]["active"], d["stream"]["owner"]=="'$U'"')
check "REST: group 1 stream.active true again with the same owner ($G), as the TV would read it" \
  "$([ "$G" = "True True" ] && grep -q 'stream.active=true owner=.* restored' "$S.log"; echo $?)"
check "no OLD-session line, file used up (deleted)" "$(! grep -q 'OLD session' "$S.log" && [ ! -e "$F" ]; echo $?)"

echo "== B) restart (as r r) =="
N=$(grep -c 'first frame decrypted with the saved session' "$S.log")
post_local /api/restart '' >/dev/null; sleep 3.5
check "restart: continued again ($(hue))" \
  "$([ "$(grep -c 'first frame decrypted with the saved session' "$S.log")" = $((N+1)) ] && [ "$(hue)" = live ]; echo $?)"

echo "== C) exit (Ctrl+C) and a fresh start (as ambi restart) =="
kill -INT $BP; sleep 1.5; kill -9 $BP 2>/dev/null
check "exit: session saved" "$([ -e "$F" ]; echo $?)"
N=$(grep -c 'first frame decrypted with the saved session' "$S.log")
start; sleep 2
check "new process: continued ($(hue)), no OLD-session line" \
  "$([ "$(grep -c 'first frame decrypted with the saved session' "$S.log")" = $((N+1)) ] && [ "$(hue)" = live ] && ! grep -q 'OLD session' "$S.log"; echo $?)"

echo "== D) crash (kill -9): nothing saved -> old session as before =="
kill -9 $BP; sleep 0.5
check "kill -9: no saved file" "$([ ! -e "$F" ]; echo $?)"
start; sleep 3
check "old-session detected as before" "$(grep -q 'streaming from an OLD session' "$S.log"; echo $?)"

echo "== E) a corrupt saved file =="
kill -9 $BP; sleep 0.5
printf 'AMBIDTLS1\n\x7f\x00\x00\x01\x00\x00garbage-garbage-garbage' > "$F"
cp "$S.log" "$S.log.before_e"; : > "$S.log"
start; sleep 3
check "corrupt file: not used, deleted, bridge fine (old-session detection, REST answers)" \
  "$([ ! -e "$F" ] && kill -0 $BP && curl -sf http://127.0.0.1:$R/api/config >/dev/null && ! grep -q 'saved session continued' "$S.log"; echo $?)"
kill $SP 2>/dev/null; wait $SP 2>/dev/null
kill -INT $BP; sleep 1; kill -9 $BP 2>/dev/null
echo
[ $RC = 0 ] && echo "v029_test: PASS" || echo "v029_test: FAIL"
exit $RC
