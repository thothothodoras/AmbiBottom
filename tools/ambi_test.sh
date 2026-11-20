#!/bin/bash
# Test for tools/ambi without sudo and without touching the real state dir:
# start through "ambi on" on test ports, stop with "ambi off", expect a clean
# exit (rc 0); "ambi off" when nothing runs must say so and return 1;
# "ambi restart" works both from off and from on (v0.19).
T=$(cd "$(dirname "$0")" && pwd)
export AMBI_SUDO= AMBI_STATE_DIR=/tmp/ambi_test_state
export AMBI_BASE_ARGS="--port 18085 --stream-port 12105 --monitor-port 18102 --pairing auto"
rm -rf /tmp/ambi_test_state
RC=0
"$T/ambi" on --console quiet > /tmp/ambi_test.log 2>&1 &
AP=$!
sleep 1.5
curl -s -o /dev/null -w "REST reply: %{http_code}\n" http://127.0.0.1:18085/api/config
"$T/ambi" off || RC=1
wait $AP; BRC=$?
echo "bridge exit code after 'ambi off': $BRC"
[ $BRC = 0 ] || RC=1
"$T/ambi" off && RC=1
# v0.19: "ambi restart" when off starts it; when on, stops + starts a new one.
"$T/ambi" restart --console quiet >> /tmp/ambi_test.log 2>&1 &
AP=$!; sleep 1.5
P1=$(head -n1 /tmp/ambi_test_state/ambi_bridge.lock)
C=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:18085/api/config)
echo "restart from off: pid $P1, REST $C"; [ "$C" = 200 ] || RC=1
"$T/ambi" restart --console quiet >> /tmp/ambi_test.log 2>&1 &
AP2=$!; sleep 2.5
wait $AP; echo "old bridge exit code after restart: $?"
P2=$(head -n1 /tmp/ambi_test_state/ambi_bridge.lock)
C=$(curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:18085/api/config)
echo "restart from on: pid $P1 -> $P2, REST $C"; { [ "$C" = 200 ] && [ "$P1" != "$P2" ]; } || RC=1
"$T/ambi" off >/dev/null || RC=1
wait $AP2
grep -c "ambi_bridge" /proc/[0-9]*/comm 2>/dev/null | grep -v ":0" | head -1
[ $RC = 0 ] && echo "ambi_test: PASS" || echo "ambi_test: FAIL"
exit $RC
