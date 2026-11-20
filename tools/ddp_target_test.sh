#!/bin/bash
# v0.32 Mode B: the bridge with --ddp-target sends DDP to a stock WLED. No TV
# and no WLED here: a real DTLS client (tools/sender) streams a still picture,
# tools/ddp_listener.py stands in for WLED's UDP port and records every
# datagram, tools/ddp_check.py checks them byte by byte (header, sequence,
# length, rate, and that the payload is exactly the monitor's output.leds).
# Then: without --ddp-target nothing may arrive; and bad targets are refused.
# Expect PASS. SSDP/mDNS are off (AMBI_NO_DISCOVERY).
# Don't launch it from a shell whose own command line contains "state-dir /tmp/e2e_ddpt".
#
# usage: tools/ddp_target_test.sh [bridge binary]
T=$(cd "$(dirname "$0")" && pwd)
BIN=${1:-$T/../build/ambi_bridge}
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
export AMBI_NO_DISCOVERY=1
R=http://127.0.0.1:18531; M=http://127.0.0.1:18539; RC=0
run() {  # $1 = extra bridge args, $2 = packets file, $3 = state file
  pkill -9 -f "state-dir /tmp/e2e_ddpt" 2>/dev/null; sleep 0.4; rm -rf /tmp/e2e_ddpt
  python3 "$T/ddp_listener.py" --port 14148 --seconds 6 --out "$2" & local LP=$!
  "$BIN" --state-dir /tmp/e2e_ddpt --port 18531 --stream-port 12531 --monitor-port 18539 --console quiet --pairing auto $1 > /tmp/e2e_ddpt.log 2>&1 &
  local BP=$!; sleep 1.5
  local RESP U K
  RESP=$(curl -s -X POST $R/api -d '{"devicetype":"e2e#ddpt","generateclientkey":true}')
  U=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["username"])')
  K=$(echo "$RESP" | python3 -c 'import sys,json;print(json.load(sys.stdin)[0]["success"]["clientkey"])')
  # Over the current budget the limiter's peaks move slowly on purpose (peak
  # drift), so frames of a still picture differ. For the byte-for-byte checks
  # the limit is taken out of play; the limit itself is tested in ctest and
  # v030_test. v0.33: the budget has a ceiling (900 mA), so "no limit" = LEDs
  # too frugal to reach it (1 mA per LED, no idle draw).
  curl -s -X PUT $R/ambi/settings -d '{"current_budget_ma":900,"ma_per_led_full_white":1,"idle_ma_per_led":0}' >/dev/null
  curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":false}}' >/dev/null
  curl -s -X PUT $R/api/$U/groups/1 -d '{"stream":{"active":true}}' >/dev/null
  "$T/sender" "$U" "$K" 12531 4 >/dev/null & local SP=$!
  sleep 3; curl -s $M/api/state > "$3"
  wait $SP; wait $LP
  kill -INT $BP; sleep 0.8; kill -9 $BP 2>/dev/null
}
echo "== 1) --ddp-target 127.0.0.1:14148 =="
run "--ddp-target 127.0.0.1:14148" /tmp/ddpt_on.json /tmp/ddpt_state.json
python3 "$T/ddp_check.py" /tmp/ddpt_on.json --leds 43 --state /tmp/ddpt_state.json || RC=1
grep -q '127.0.0.1:14148' /tmp/ddpt_state.json && echo "[ok]   the monitor state names the target" || { echo "[FAIL] the monitor state does not name the target"; RC=1; }
echo "== 2) no --ddp-target (the default): nothing may be sent =="
run "" /tmp/ddpt_off.json /tmp/ddpt_state_off.json
python3 "$T/ddp_check.py" /tmp/ddpt_off.json --none || RC=1
echo "== 3) the old spelling still works; bad targets are refused at start =="
run "--wled-ip 127.0.0.1 --wled-port 14148" /tmp/ddpt_old.json /tmp/ddpt_state_old.json
python3 "$T/ddp_check.py" /tmp/ddpt_old.json --leds 43 > /dev/null && echo "[ok]   --wled-ip/--wled-port give the same packets" || { echo "[FAIL] --wled-ip/--wled-port"; RC=1; }
for BAD in "not-an-ip" "127.0.0.1:0" "127.0.0.1:99999"; do
  timeout 5 "$BIN" --state-dir /tmp/e2e_ddpt --port 18531 --stream-port 12531 --monitor-port 18539 --console quiet --ddp-target "$BAD" > /tmp/e2e_ddpt_bad.log 2>&1
  [ $? = 2 ] && echo "[ok]   --ddp-target $BAD: refused (exit 2)" || { echo "[FAIL] --ddp-target $BAD was not refused"; RC=1; }
done
echo; [ $RC = 0 ] && echo "ddp_target_test: PASS" || echo "ddp_target_test: FAIL"
exit $RC
