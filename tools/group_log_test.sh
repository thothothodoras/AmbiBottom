#!/bin/bash
# v0.22 end-to-end test, no TV needed: the log that helps catch "Immersive greyed out".
#  1) every GET /groups/N says what the bridge answered (Entertainment / not found);
#  2) groups the TV creates are stored with their creation time;
#  3) the log file starts with a dated header, and the previous run's log is kept
#     as ambi_bridge.log.1 when the bridge starts again.
# The log file only exists in dashboard mode, so the bridge runs on a pty (script).
#
# usage: tools/group_log_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains
# "state-dir /tmp/e2e_glog" (the pkill below would kill that shell).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
M=18120; R=18102; S=/tmp/e2e_glog
RC=0
check() {  # $1 = description, $2 = condition result (0 = ok)
  if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi
}
start() {
  script -qec "stty cols 110 rows 30; exec $BIN --state-dir $S --port $R --stream-port 12122 --monitor-port $M --pairing auto" /dev/null > /dev/null 2>&1 &
  sleep 2
}
stop() { pkill -INT -f "state-dir $S" 2>/dev/null; sleep 1.5; pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.3; }

pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S"; mkdir -p "$S"

echo "== run 1 =="
start
U=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"glog#test"}' | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['success']['username'])")
curl -s -X POST http://127.0.0.1:$R/api/$U/groups -d '{"type":"Entertainment","class":"TV","lights":["1","2"]}' > /dev/null
curl -s http://127.0.0.1:$R/api/$U/groups/1 > /dev/null
curl -s http://127.0.0.1:$R/api/$U/groups/99 > /dev/null
sleep 0.5
L=$S/ambi_bridge.log
check "log starts with a dated header" "$(head -1 "$L" | grep -Eq '^=== log started [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9:]{8} \(ambi-bridge-cpp v0\.' ; echo $?)"
check "GET groups/1 logged as answered Entertainment" "$(grep -q "asked groups/1 -> answered Entertainment 'TV'" "$L"; echo $?)"
check "GET groups/99 logged as NOT FOUND" "$(grep -q 'asked groups/99 -> answered NOT FOUND' "$L"; echo $?)"
check "created group stored with a creation time" \
  "$(python3 -c "import json,sys;d=json.load(open('$S/groups.json'));sys.exit(0 if any(isinstance(g,dict) and len(g.get('created',''))==19 for g in d.values()) else 1)"; echo $?)"
check "the new group's own lookup logs its creation time" \
  "$(curl -s http://127.0.0.1:$R/api/$U/groups/2 > /dev/null; sleep 0.3; grep -Eq "asked groups/2 -> answered Entertainment 'TV' \(created [0-9]{4}-" "$L"; echo $?)"
check "no previous log yet (.1 absent on the first run)" "$([ ! -e "$L.1" ]; echo $?)"
stop

echo "== run 2 =="
start
check "previous run's log kept as ambi_bridge.log.1" "$(grep -q 'asked groups/99 -> answered NOT FOUND' "$L.1"; echo $?)"
check "the new ambi_bridge.log is fresh (no groups/99 line)" "$(! grep -q 'groups/99' "$L"; echo $?)"
check "the new log has its own header" "$(head -1 "$L" | grep -q '^=== log started'; echo $?)"
stop
rm -rf "$S"
[ $RC = 0 ] && echo PASS || echo FAIL
exit $RC
