#!/bin/bash
# v0.45: JointSpace pairing from the page (against tools/mock_tv.py, which plays a TV that shows
# PIN 4321) and the two page languages, against the simulator (build/esp_app_sim = the
# firmware's App code on Linux). Loopback only, test ports, nothing on the LAN.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18497; SPORT=21907; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
st() { curl -s -m 3 $H/ambi/api/state; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
code() { curl -s -o "$D/out.json" -w '%{http_code}' -XPOST -d "$2" "$H$1"; }
pair() { curl -s $H/ambi/api/jointspace/pair; }
wait_state() {  # wait (max 5 s) until the pairing state is not $1
  for _ in $(seq 50); do [ "$(pair | js 'd["state"]')" != "$1" ] && return 0; sleep 0.1; done; return 1; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
[ -x "$T/sender" ] || g++ -std=c++17 "$T/sender.cpp" -o "$T/sender" -lmbedtls -lmbedx509 -lmbedcrypto || exit 1
[ -f "$T/cert.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" -days 365 -subj "/CN=tv" >/dev/null 2>&1
(cd "$T" && exec python3 mock_tv.py 1926 > "$D/mock.log" 2>&1) & MOCK=$!
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8

check "pairing: idle at first, JointSpace not configured" "[ \"\$(pair | js 'd[\"state\"]')\" = idle ] && [ \"\$(st | js 'd[\"sources\"][\"jointspace\"][\"status\"]')\" = not_configured ]"
check "pairing: a PIN with nothing waiting -> 409" "[ \$(code /ambi/api/jointspace/pair/pin '{\"pin\":\"4321\"}') = 409 ]"
check "pairing: a TV address that is not an IP -> 409 tv_address_unknown" "[ \$(code /ambi/api/jointspace/pair '{\"tv_ip\":\"tv.local\"}') = 409 ] && grep -q tv_address_unknown $D/out.json"
check "pairing: start -> 200" "[ \$(code /ambi/api/jointspace/pair '{\"tv_ip\":\"127.0.0.1\"}') = 200 ]"
wait_state requesting
check "pairing: the TV was asked and shows a PIN -> waiting_pin, timeout 60 s" "[ \"\$(pair | js 'd[\"state\"],0<d[\"timeout_s\"]<=60')\" = 'waiting_pin True' ] && grep -q tv_shows_pin $D/mock.log"
check "pairing: PIN with letters -> 422" "[ \$(code /ambi/api/jointspace/pair/pin '{\"pin\":\"12ab\"}') = 422 ]"
code /ambi/api/jointspace/pair/pin '{"pin":"1111"}' > /dev/null; wait_state granting
check "pairing: wrong PIN -> still waiting_pin, error invalid_pin (can be typed again)" "[ \"\$(pair | js 'd[\"state\"],d[\"error\"]')\" = 'waiting_pin invalid_pin' ]"
code /ambi/api/jointspace/pair/pin '{"pin":"4321"}' > /dev/null; wait_state granting
check "pairing: right PIN -> done" "[ \"\$(pair | js 'd[\"state\"],d[\"error\"]')\" = 'done ' ]"
sleep 1.2  # mock_tv.py writes its counters once a second
check "pairing: the TV granted it (signature + Digest accepted)" "grep '^{\"connections' $D/mock.log | tail -1 | js 'd[\"pair_grants_ok\"]==1 and d[\"pair_grants_bad\"]==1' | grep -q True"
check "pairing: credentials saved (configured), never in the replies" "curl -s $H/ambi/api/settings/jointspace | js 'd[\"configured\"]' | grep -q True && ! (pair; curl -s $H/ambi/api/settings/jointspace) | grep -qiE 'auth_key|password\":\"[^\"]'"
# the TV streams Hue: JointSpace must work with the paired credentials
curl -s -XPOST -d '{"action":"start"}' $H/ambi/api/service > /dev/null
curl -s -XPOST $H/sim/press > /dev/null
RESP=$(curl -s -XPOST $H/api -d '{"devicetype":"v045#test","generateclientkey":true}')
U=$(echo "$RESP" | js 'd[0]["success"]["username"]'); K=$(echo "$RESP" | js 'd[0]["success"]["clientkey"]')
G=$(curl -s -XPOST $H/api/$U/groups -d '{"lights":["1","2","3"],"type":"Entertainment","class":"TV"}' | js 'd[0]["success"]["id"]')
curl -s -XPUT $H/api/$U/groups/$G -d '{"stream":{"active":true}}' > /dev/null
SENDER_HOST=127.0.0.1 SENDER_LAMPS=3 "$T/sender" "$U" "$K" $SPORT 9 > "$D/sender.log" 2>&1 & SP=$!
sleep 6.5  # v0.47: JointSpace waits 4 s after the stream starts
check "JointSpace live with the paired credentials (zones 3/8/3)" "[ \"\$(st | js 'd[\"sources\"][\"jointspace\"][\"status\"],d[\"topology\"][\"left\"],d[\"topology\"][\"top\"],d[\"topology\"][\"right\"]')\" = 'live 3 8 3' ]"
wait $SP 2>/dev/null
grep -qE "[0-9a-f]{32}" "$D/sim.log" && bad "a 32-hex value (device id / key) in the log" || ok "no device id / key in the log"
# pages
check "page /ambi: English (lang en, no Greek apart from the ΕΛ button)" "curl -s $H/ambi | python3 -c \"import re,sys;p=sys.stdin.read();g=re.findall(r'[\\u0370-\\u03FF]+',p);sys.exit(0 if '<html lang=\\\"en\\\">' in p and set(g)<={'Ελληνικά','ΕΛ'} else 1)\""
# v0.49: /ambi/el is a loader + patch; the page the browser ends up with = ambi_page_el.html (v049_test proves it)
EL="$R/firmware/wled-usermod/AmbiBottom/ambi_page_el.html"
check "page /ambi/el: Greek, with the EN button" "curl -s $H/ambi/el | grep -q '<html lang=\"el\">' && grep -q '<html lang=\"el\">' $EL && grep -q 'hreflang=\"en\"' $EL"
check "page: JointSpace pairing controls in both languages" "curl -s $H/ambi | grep -q 'id=\"jsPairStart\"' && curl -s $H/ambi | grep -q 'Pair with the TV' && grep -q 'Pair με την TV' $EL"
check "page: all 25 WLED time zones" "[ \$(curl -s $H/ambi | python3 -c \"import re,sys;p=sys.stdin.read();s=p[p.index('id=\\\"devTz\\\"'):];s=s[:s.index('</select>')];print(len(re.findall('<option',s)))\") = 25 ]"
check "the ESP page files are up to date" "python3 $T/make_esp_page.py --check > /dev/null"

kill $SIMPID $MOCK 2>/dev/null; wait $SIMPID $MOCK 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
