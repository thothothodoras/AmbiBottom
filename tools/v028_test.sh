#!/bin/bash
# v0.28 end to end, without a TV:
#  A) /api/state: service running + controls; peak_points field on the page and in the settings
#  B) Stop / Start from the page (local): same pid, REST + DTLS + SSDP ports free while stopped, page stays;
#     LAN without login refused
#  C) Login: set only locally (0600 file, no plain password), LAN edits need it, cookie flags, CSRF (Origin),
#     wrong password -> 401, 5 wrong -> 429 locked; the session survives Stop/Start (execv); logout; new password
#     logs everyone out; removing the login
#  D) Terminal keys on a pty: q = stop (terminal stays), q again = "already stopped", σ (Greek s) = start, x = exit
#  E) Exit from the page: the process ends, ports free
# Same pkill caution as the other tests: never run a command line containing "state-dir /tmp/e2e_v28".
T=$(cd "$(dirname "$0")" && pwd)
SRC_BIN=${1:-$T/../build/ambi_bridge}
S=/tmp/e2e_v28
B=/tmp/e2e_v28bin
R=18095; D=12135; M=18135
RC=0
check() { if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi; }
json() { python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"; }
LAN=$(hostname -I | awk '{print $1}')
J="$S.jar"
post_local() { curl -s -X POST "http://127.0.0.1:$M$1" -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' -d "$2"; }
code_local() { curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$M$1" -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' -d "$2"; }
# LAN request with the cookie jar and the right Origin
code_lan() { curl -s -o /dev/null -w '%{http_code}' -b "$J" -c "$J" -X POST "http://$LAN:$M$1" -H "Origin: http://$LAN:$M" -H 'Content-Type: application/json' -d "$2"; }
get_lan() { curl -s -b "$J" "http://$LAN:$M$1"; }
port_free_tcp() { python3 -c "import socket,sys;s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('0.0.0.0',$1));print(0)" 2>/dev/null || echo 1; }
port_free_udp() { python3 -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('0.0.0.0',$1));print(0)" 2>/dev/null || echo 1; }
svc() { curl -s http://127.0.0.1:$M/api/state | json 'd.get("service")'; }

pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S" "$S.log" "$S"2 "$B" "$J"; mkdir -p "$B"
cp "$SRC_BIN" "$B/ambi_bridge"
"$B/ambi_bridge" --state-dir "$S" --port $R --stream-port $D --monitor-port $M --console quiet --pairing auto --lamps 5 \
  >> "$S.log" 2>&1 &
BP=$!; sleep 1.5
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#v28","generateclientkey":true}')
U=$(echo "$RESP" | json 'd[0]["success"]["username"]')

echo "== A) state + peak_points =="
check "state: service running, service_controls true" \
  "$(curl -s http://127.0.0.1:$M/api/state | python3 -c 'import sys,json;d=json.load(sys.stdin);sys.exit(0 if d["service"]=="running" and d["service_controls"] is True else 1)'; echo $?)"
check "page: Start/Stop/Restart/Exit buttons, login button + dialog, access card, peak_points field" \
  "$(P=$(curl -s http://127.0.0.1:$M/); for id in btnStart btnStop btnRestart btnExit loginButton loginDialog accessPanel setPeakPoints serviceBanner; do echo "$P" | grep -q "id=\"$id\"" || exit 1; done; echo 0)"
P=$(post_local /api/settings/strip '{"peak_points":4}')
check "peak_points 4 saved from the local page" "$(echo "$P" | python3 -c 'import sys,json;sys.exit(0 if json.load(sys.stdin)["settings"]["peak_points"]==4 else 1)'; echo $?)"
post_local /api/settings/strip '{"peak_points":0}' >/dev/null

echo "== B) Stop / Start from the page =="
C=$(code_lan /api/service '{"action":"stop"}')
check "stop from the LAN without login refused ($C)" "$([ "$C" = 403 ]; echo $?)"
C=$(post_local /api/service '{"action":"stop"}' | json 'd.get("ok")')
sleep 2.5
check "stop: ok=$C, same pid, monitor says stopped" "$([ "$C" = True ] && kill -0 $BP 2>/dev/null && [ "$(svc)" = stopped ]; echo $?)"
check "stopped: REST port $R, DTLS udp $D free (nothing listens)" \
  "$([ "$(port_free_tcp $R)" = 0 ] && [ "$(port_free_udp $D)" = 0 ] && ! curl -sf -m 1 http://127.0.0.1:$R/api/config >/dev/null; echo $?)"
check "log: SERVICE STOPPED banner + 'stop requested from monitor page'" \
  "$(grep -q 'SERVICE STOPPED: no REST' "$S.log" && grep -q 'stop requested from monitor page (127.0.0.1)' "$S.log"; echo $?)"
C=$(post_local /api/service '{"action":"stop"}' | json 'd.get("error")')
check "stop again: already_stopped ($C)" "$([ "$C" = already_stopped ]; echo $?)"
C=$(post_local /api/service '{"action":"start"}' | json 'd.get("ok")')
sleep 2.5
check "start: ok=$C, same pid, running, REST answers, pairing kept" \
  "$([ "$C" = True ] && kill -0 $BP 2>/dev/null && [ "$(svc)" = running ] && curl -s http://127.0.0.1:$R/api/$U/lights | python3 -c 'import sys,json;sys.exit(0 if len(json.load(sys.stdin))==5 else 1)'; echo $?)"
C=$(post_local /api/service '{"action":"start"}' | json 'd.get("error")')
check "start again: already_running ($C)" "$([ "$C" = already_running ]; echo $?)"

echo "== C) Login =="
L=$(get_lan /api/login)
check "LAN: no login set, cannot edit" "$(echo "$L" | python3 -c 'import sys,json;d=json.load(sys.stdin);sys.exit(0 if d["configured"] is False and d["can_edit"] is False and d["local"] is False else 1)'; echo $?)"
C=$(code_lan /api/login/setup '{"action":"set","username":"me","password":"secret-123"}')
check "setting the login from the LAN refused ($C)" "$([ "$C" = 403 ]; echo $?)"
C=$(post_local /api/login/setup '{"action":"set","username":"me","password":"short"}' | json 'd.get("error")')
check "short password refused ($C)" "$([ "$C" = password_min_8_chars ]; echo $?)"
post_local /api/login/setup '{"action":"set","username":"me","password":"secret-123"}' >/dev/null
check "monitor_login.json: mode 600, pbkdf2, no plain password" \
  "$([ "$(stat -c %a "$S/monitor_login.json")" = 600 ] && grep -q pbkdf2-hmac-sha256 "$S/monitor_login.json" && ! grep -q secret-123 "$S/monitor_login.json"; echo $?)"
C=$(code_lan /api/settings/strip '{"rise_ms":150}')
check "LAN edit without login refused ($C)" "$([ "$C" = 403 ]; echo $?)"
C=$(code_lan /api/login '{"username":"me","password":"wrong-pass"}')
check "wrong password -> 401 ($C)" "$([ "$C" = 401 ]; echo $?)"
H=$(curl -s -D - -o /dev/null -c "$J" -X POST "http://$LAN:$M/api/login" -H "Origin: http://$LAN:$M" -d '{"username":"me","password":"secret-123"}')
check "login ok: Set-Cookie ambi_session, HttpOnly, SameSite=Strict, 7 days" \
  "$(echo "$H" | grep -i '^Set-Cookie: ambi_session=' | grep -q 'HttpOnly; SameSite=Strict; Max-Age=604800'; echo $?)"
check "LAN logged in: can_edit, strip settings editable" \
  "$(get_lan /api/login | json 'd["logged_in"] and d["can_edit"] and d["username"]=="me"' | grep -q True && get_lan /api/settings/strip | json 'd["can_edit"]' | grep -q True; echo $?)"
C=$(code_lan /api/settings/strip '{"rise_ms":150}')
check "LAN edit with login -> 200 ($C)" "$([ "$C" = 200 ] && grep -q 'strip settings changed from the page' "$S.log"; echo $?)"
C=$(curl -s -o /dev/null -w '%{http_code}' -b "$J" -X POST "http://$LAN:$M/api/settings/strip" -H "Origin: http://evil.example" -d '{"rise_ms":160}')
check "logged in but foreign Origin (CSRF) refused ($C)" "$([ "$C" = 403 ]; echo $?)"
C=$(code_lan /api/login/setup '{"action":"clear"}')
check "a logged-in device cannot change/remove the login ($C)" "$([ "$C" = 403 ]; echo $?)"
C=$(code_lan /api/service '{"action":"stop"}'); sleep 2.5
check "stop from the LAN with login ($C), stopped" "$([ "$C" = 200 ] && [ "$(svc)" = stopped ]; echo $?)"
check "session survives the stop (execv): still logged in" "$(get_lan /api/login | json 'd["logged_in"]' | grep -q True; echo $?)"
C=$(code_lan /api/service '{"action":"start"}'); sleep 2.5
check "start from the LAN with login ($C), running" "$([ "$C" = 200 ] && [ "$(svc)" = running ]; echo $?)"
cp "$J" "$J.old"
for i in 1 2 3 4 5; do curl -s -o /dev/null -X POST "http://$LAN:$M/api/login" -H "Origin: http://$LAN:$M" -d '{"username":"me","password":"bad"}'; done
C=$(curl -s -X POST "http://$LAN:$M/api/login" -H "Origin: http://$LAN:$M" -d '{"username":"me","password":"secret-123"}' -w ' %{http_code}')
check "5 wrong -> locked, even the right password waits: $C" "$(echo "$C" | grep -q '"locked".* 429$'; echo $?)"
check "log: login refused lines" "$([ "$(grep -c "login refused from $LAN" "$S.log")" -ge 6 ]; echo $?)"
code_lan /api/logout '{}' >/dev/null
check "logout: no longer logged in" "$(get_lan /api/login | json 'd["logged_in"]' | grep -q False; echo $?)"
C=$(curl -s -o /dev/null -w '%{http_code}' -b "$J.old" -X POST "http://$LAN:$M/api/settings/strip" -H "Origin: http://$LAN:$M" -d '{"rise_ms":170}')
check "the logged-out cookie no longer works ($C)" "$([ "$C" = 403 ]; echo $?)"
curl -s -c "$J" -X POST "http://127.0.0.1:$M/api/login" -H "Origin: http://127.0.0.1:$M" -d '{"username":"me","password":"secret-123"}' >/dev/null
post_local /api/login/setup '{"action":"set","username":"me","password":"another-456"}' >/dev/null
C=$(curl -s -o /dev/null -w '%{http_code}' -b "$J" -X POST "http://$LAN:$M/api/settings/strip" -H "Origin: http://$LAN:$M" -d '{"rise_ms":170}')
check "new password: earlier sessions logged out ($C)" "$([ "$C" = 403 ]; echo $?)"
post_local /api/login/setup '{"action":"clear"}' >/dev/null
check "login removed: files gone, LAN sees configured=false" \
  "$([ ! -e "$S/monitor_login.json" ] && get_lan /api/login | json 'd["configured"]' | grep -q False; echo $?)"

echo "== E) Exit from the page =="
C=$(post_local /api/service '{"action":"exit"}' | json 'd.get("ok")')
sleep 2
check "exit: ok=$C, process gone, monitor + REST ports free" \
  "$([ "$C" = True ] && ! kill -0 $BP 2>/dev/null && [ "$(port_free_tcp $M)" = 0 ] && [ "$(port_free_tcp $R)" = 0 ]; echo $?)"
wait $BP 2>/dev/null

echo "== D) Terminal keys (pty): q stop, q again, σ start, x exit =="
S2="$S"2
( sleep 3; printf q; sleep 3; printf q; sleep 1; printf 'σ'; sleep 3.5; printf x; sleep 3 ) | \
  script -qec "stty cols 140 rows 30; exec $B/ambi_bridge --state-dir $S2 --port $R --stream-port $D --monitor-port $M --pairing auto" /dev/null > "$S.pty" 2>&1 &
SCP=$!
sleep 5
check "q: stopped, terminal stays (monitor answers), REST down" \
  "$([ "$(svc)" = stopped ] && ! curl -sf -m 1 http://127.0.0.1:$R/api/config >/dev/null; echo $?)"
sleep 2
LS=$(cat "$S2"/ambi_bridge.log* 2>/dev/null)
check "q again: 'already stopped - s starts it, x exits'" "$(echo "$LS" | grep -q 'already stopped - s starts it, x exits'; echo $?)"
check "footer while stopped: 'SERVICE STOPPED  s: start  r x2: restart  x: exit'" \
  "$(grep -aq 'SERVICE STOPPED  s: start  r x2: restart  x: exit' "$S.pty"; echo $?)"
sleep 2.5
check "σ (Greek s): started again, REST answers" "$([ "$(svc)" = running ] && curl -sf http://127.0.0.1:$R/api/config >/dev/null; echo $?)"
check "footer while running: 'q: stop  s: start  r x2: restart  x: exit  p: pair'" \
  "$(grep -aq 'q: stop  s: start  r x2: restart  x: exit  p: pair' "$S.pty"; echo $?)"
sleep 4
check "x: the program exited, monitor port free" \
  "$(! curl -sf -m 1 http://127.0.0.1:$M/api/state >/dev/null && [ "$(port_free_tcp $M)" = 0 ]; echo $?)"
pkill -INT -f "state-dir $S2" 2>/dev/null; sleep 1; pkill -9 -f "state-dir $S2" 2>/dev/null; kill $SCP 2>/dev/null; wait $SCP 2>/dev/null

[ $RC = 0 ] && echo "v028_test: PASS" || echo "v028_test: FAIL"
exit $RC
