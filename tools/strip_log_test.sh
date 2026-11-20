#!/bin/bash
# v0.23 end-to-end test, no TV needed: a real DTLS stream over a tiny budget.
#  - NEXT 6: /api/strip_stats counts streaming/over-budget time per LED, groups
#    the over-budget stretches into episodes (wanted -> sent, peaks kept),
#    reset clears only "since reset" (local browser only), strip_stats.json
#    survives a restart.
#  - NEXT 5: the peaks drift while over budget (episode drift range > 0).
#  - NEXT 9: blend_softness reaches the output (limit.blend_softness).
#
# usage: tools/strip_log_test.sh [bridge binary]
# Don't launch it from a shell whose own command line contains
# "state-dir /tmp/e2e_slog" (the pkill below would kill that shell).
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
LAN=$(hostname -I | awk '{print $1}')
M=18130; R=18112; P=12132; S=/tmp/e2e_slog
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
RC=0
check() {  # $1 = description, $2 = condition result (0 = ok)
  if [ "$2" = 0 ]; then echo "[ok]   $1"; else echo "[FAIL] $1"; RC=1; fi
}
py() { python3 -c "import sys,json;d=json.load(sys.stdin);$1"; }
start() {
  "$BIN" --state-dir "$S" --port $R --stream-port $P --monitor-port $M --console quiet --pairing auto >> "$S.log" 2>&1 &
  sleep 1.5
}
stop() { pkill -INT -f "state-dir $S" 2>/dev/null; sleep 1.5; pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.3; }

pkill -9 -f "state-dir $S" 2>/dev/null; sleep 0.5; rm -rf "$S" "$S.log"
start
curl -s -X POST http://127.0.0.1:$M/api/settings/strip -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' \
  -d '{"current_budget_ma":120,"peak_emphasis":0.8,"peak_drift_leds":3,"peak_drift_period_s":10,"blend_mode":"zones","blend_softness":0.3,"rise_ms":0}' > /dev/null
RESP=$(curl -s -X POST http://127.0.0.1:$R/api -d '{"devicetype":"e2e#slog","generateclientkey":true}')
USER=$(echo "$RESP" | py 'print(d[0]["success"]["username"])'); KEY=$(echo "$RESP" | py 'print(d[0]["success"]["clientkey"])')
curl -s -X PUT http://127.0.0.1:$R/api/$USER/groups/1 -d '{"stream":{"active":true}}' > /dev/null

echo "== two over-budget stretches, 3 s apart =="
"$T/sender" "$USER" "$KEY" $P 4 > /dev/null
O=$(curl -s http://127.0.0.1:$M/api/state)
sleep 3
"$T/sender" "$USER" "$KEY" $P 3 > /dev/null
sleep 2
check "output carries blend_softness 0.3 and peak_drift_leds 3" \
  "$(echo "$O" | py 'l=d["output"]["limit"];sys.exit(0 if l["blend_softness"]==0.3 and l["peak_drift_leds"]==3 else 1)'; echo $?)"
check "output: wanted > budget, sent <= budget, mode peaks" \
  "$(echo "$O" | py 'o=d["output"];l=o["limit"];sys.exit(0 if l["mode"]=="peaks" and l["wanted_ma"]>120 and o["estimated_ma"]<=120 else 1)'; echo $?)"
J=$(curl -s http://127.0.0.1:$M/api/strip_stats)
check "2 episodes, both closed, wanted > 120 and sent <= 120" \
  "$(echo "$J" | py 'e=d["episodes"];sys.exit(0 if len(e)==2 and not any(x["open"] for x in e) and all(x["wanted_ma"]>120 and x["sent_ma"]<=120 for x in e) else 1)'; echo $?)"
check "episodes record the peaks kept (3 LEDs) and a lower level for the rest" \
  "$(echo "$J" | py 'e=d["episodes"][0];sys.exit(0 if len(e["peak_leds"])==3 and e["min_scale"]<e["peak_scale"] else 1)'; echo $?)"
check "rolling peaks: the drift moved during the 4 s stretch (period 10 s)" \
  "$(echo "$J" | py 'e=d["episodes"][1];sys.exit(0 if e["drift_max"]-e["drift_min"]>1.0 else 1)'; echo $?)"
check "since_reset: ~7 s streamed, all of it over budget, 43 LEDs" \
  "$(echo "$J" | py 't=d["since_reset"];sys.exit(0 if 5.5<t["stream_s"]<8 and t["over_s"]>5 and len(t["mah"])==43 else 1)'; echo $?)"
check "peak LEDs have peak time, mAh > 0 on every LED" \
  "$(echo "$J" | py 't=d["since_reset"];pk=d["episodes"][0]["peak_leds"];sys.exit(0 if sum(t["s_peak"])>0 and all(m>0 for m in t["mah"]) else 1)'; echo $?)"
check "one day in the 30-day history with 2 episodes" \
  "$(echo "$J" | py 'x=d["days"];sys.exit(0 if len(x)==1 and x[0]["episodes"]==2 else 1)'; echo $?)"
C=$(curl -s -o /dev/null -w '%{http_code}' -X POST http://$LAN:$M/api/strip_stats/reset -H "Origin: http://$LAN:$M")
check "reset from the LAN refused (403)" "$([ "$C" = 403 ]; echo $?)"
J2=$(curl -s -X POST http://127.0.0.1:$M/api/strip_stats/reset -H "Origin: http://127.0.0.1:$M" -H 'Content-Type: application/json' -d '{}')
check "reset from the local page: since_reset empty, all_time and episodes kept" \
  "$(echo "$J2" | py 'sys.exit(0 if d["since_reset"]["frames"]==0 and d["all_time"]["frames"]>100 and len(d["episodes"])==2 else 1)'; echo $?)"
FR=$(echo "$J2" | py 'print(d["all_time"]["frames"])')
stop
check "strip_stats.json written at exit" "$([ -s "$S/strip_stats.json" ]; echo $?)"
echo "== restart =="
start
J3=$(curl -s http://127.0.0.1:$M/api/strip_stats)
check "after a restart: same all-time frames ($FR), 2 episodes, since_reset still empty" \
  "$(echo "$J3" | py "sys.exit(0 if d['all_time']['frames']==$FR and len(d['episodes'])==2 and d['since_reset']['frames']==0 else 1)"; echo $?)"
stop
rm -rf "$S" "$S.log"
[ $RC = 0 ] && echo PASS || echo FAIL
exit $RC
