#!/bin/bash
# v0.46: the new settings layout, the visible brightness slider and the Ambi button patch for
# WLED (v0.48: the TV remote and the IR test were removed again - user: «βγάλ' τα εντελώς»;
# their checks with them). Was also: TV remote, IR test.
# layout and the visible brightness slider - against the simulator (build/esp_app_sim) and
# tools/mock_tv.py (a TV with keys and a power state). Loopback only, test ports.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18498; SPORT=21908; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
code() { curl -s -o "$D/out.json" -w '%{http_code}' -XPOST -d "$2" "$H$1"; }
rc() { curl -s $H/ambi/api/tv/key; }
settle() { for _ in $(seq 60); do [ "$(rc | js 'd["pending"]')" = 0 ] && sleep 0.3 && return 0; sleep 0.1; done; return 1; }
mock() { sleep 1.1; grep '^{"connections' "$D/mock.log" | tail -1; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
(cd "$T" && exec python3 mock_tv.py 1926 > "$D/mock.log" 2>&1) & MOCK=$!
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8

# pages
P=$(curl -s $H/ambi); PE=$(curl -s $H/ambi/el)
layout_ok() { python3 - "$1" <<'EOF'
import re, sys
p = open(sys.argv[1], encoding="utf-8").read()
a = p.index('class="bridge-cols"'); b = p.index('class="bridge-main"')
side = p[a:b]
sys.exit(0 if 'id="servicePanel"' in side and len(re.findall('data-goto=', side)) == 7 and 'class="bridge-grid"' not in p else 1)
EOF
}
echo "$P" > "$D/page.html"
check "page: settings = side column (bridge + index of 7 cards, v0.49: + camera) + one main column" "layout_ok $D/page.html"
check "page: brightness slider visible (not inside the folded details)" "echo \"\$P\" | python3 -c \"import sys;p=sys.stdin.read();i=p.index('id=\\\"espTvBri\\\"');sys.exit(0 if p.index('id=\\\"espMore\\\"')>i else 1)\""
check "v0.48: no remote card / no remote API any more" "! echo \"\$P\" | grep -q 'id=\"tvRemote\"' && [ \$(curl -s -o /dev/null -w '%{http_code}' $H/ambi/api/tv/key) = 404 ]"
check "the ESP page files are up to date" "python3 $T/make_esp_page.py --check > /dev/null"
W=$(mktemp -d); mkdir -p "$W/wled00/data"; printf 'REGISTER_USERMOD\n' > "$W/wled00/fcn_declare.h"
printf '<button id="buttonSr" onclick="toggleLiveview()"><i class="icons">&#xe410;</i><p class="tab-label">Peek</p></button>\n' > "$W/wled00/data/index.htm"
"$R/firmware/wled-usermod/install16.sh" "$W" > /dev/null; "$R/firmware/wled-usermod/install16.sh" "$W" > /dev/null
check "install16.sh: Ambi button added to WLED's top bar once (run twice)" "[ \$(grep -c 'id=\"buttonAmbi\"' $W/wled00/data/index.htm) = 1 ]"
printf '<div>new top bar</div>\n' > "$W/wled00/data/index.htm"
check "install16.sh: a changed WLED top bar -> stops with a message, file untouched" "! \"$R/firmware/wled-usermod/install16.sh\" \"$W\" > /dev/null 2>&1 && grep -q 'new top bar' $W/wled00/data/index.htm"
rm -rf "$W"

kill $SIMPID $MOCK 2>/dev/null; wait $SIMPID $MOCK 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
