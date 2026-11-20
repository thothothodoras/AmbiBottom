#!/bin/bash
# v0.34: which arguments the bridge gives avahi-publish-address / -service, checked by
# RUNNING it - inside a private network namespace (unshare -rn), so its SSDP answers
# can never reach the real TV, and with fake avahi-publish-* programs first on PATH
# that only log their arguments. Needs unprivileged user namespaces.
# usage: tools/mdns_args_test.sh [bridge binary]
T=$(cd "$(dirname "$0")" && pwd)
BIN=$(readlink -f "${1:-$T/../build/ambi_bridge}")
W=$(mktemp -d)
mkdir -p "$W/bin"
for p in avahi-publish-address avahi-publish-service; do
  printf '#!/bin/sh\necho "%s $*" >> "%s/args.log"\nexec sleep 30\n' "$p" "$W" > "$W/bin/$p"
  chmod +x "$W/bin/$p"
done
unshare -rn sh -c "
  ip link set lo up
  ip link add d0 type dummy && ip addr add 10.77.0.2/24 dev d0 && ip link set d0 up && ip route add default dev d0
  PATH='$W/bin':\$PATH '$BIN' --state-dir '$W/state' --port 18671 --stream-port 12671 --monitor-port 18672 \
    --console quiet --pairing auto > '$W/bridge.log' 2>&1 &
  BP=\$!; sleep 4; kill -INT \$BP; sleep 1; kill -9 \$BP 2>/dev/null
" 2>/dev/null
RC=0
echo "logged calls:"; sed 's/^/  /' "$W/args.log" 2>/dev/null
if grep -Eq '^avahi-publish-address -R [^ ]+\.local [0-9.]+$' "$W/args.log" 2>/dev/null; then
  echo "[ok]   avahi-publish-address gets -R <host>.local <ip>"
else
  echo "[FAIL] avahi-publish-address was not called with -R (bridge log: $W/bridge.log)"; RC=1
fi
grep -q '^avahi-publish-service -H ' "$W/args.log" 2>/dev/null && echo "[ok]   avahi-publish-service still started" || { echo "[FAIL] no avahi-publish-service"; RC=1; }
pkill -f "$W/bin/avahi-publish" 2>/dev/null
[ $RC = 0 ] && { rm -rf "$W"; echo "mdns_args_test: PASS"; } || echo "mdns_args_test: FAIL (kept $W)"
exit $RC
