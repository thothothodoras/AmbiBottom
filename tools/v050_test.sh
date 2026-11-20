#!/bin/bash
# v0.50: three named white-balance slots on the board (GET / POST /ambi/api/settings/wb_slots,
# kept across a restart), the page's slot block, the review fixes that show in the page (camera:
# one step per new frame, at most 40 steps; state polling with a 3 s timeout; Diagnosis compares
# WLED with wled_ma_target) - against the simulator (build/esp_app_sim). Loopback only, test ports.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18502; SPORT=21912; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8
S=$H/ambi/api/settings
code() { curl -s -o /dev/null -w '%{http_code}' -XPOST -d "$1" $S/wb_slots; }

# ---- slots API
check "three empty slots at first" "[ \"\$(curl -s $S/wb_slots | js '[x.get(\"empty\") for x in d[\"slots\"]]')\" = '[True, True, True]' ]"
curl -s -XPOST -d '{"wb_r":1,"wb_g":0.81,"wb_b":0.5}' $S/strip > /dev/null
curl -s -XPOST -d '{"slot":1,"action":"save","name":"Βράδυ"}' $S/wb_slots > /dev/null
check "save: slot 1 = the white balance in force, with its (Greek) name" "[ \"\$(curl -s $S/wb_slots | js 'd[\"slots\"][0][\"name\"],d[\"slots\"][0][\"wb\"]')\" = \"Βράδυ [1, 0.81, 0.5]\" ]"
curl -s -XPOST -d '{"wb_g":0.3,"wb_b":0.2,"wb_keep_pure":false}' $S/strip > /dev/null
curl -s -XPOST -d '{"slot":2,"action":"save"}' $S/wb_slots > /dev/null
check "save without a name: 'Slot 2', keep_pure kept (false)" "[ \"\$(curl -s $S/wb_slots | js 'd[\"slots\"][1][\"name\"],d[\"slots\"][1][\"keep_pure\"]')\" = 'Slot 2 False' ]"
curl -s -XPOST -d '{"slot":1,"action":"load"}' $S/wb_slots > /dev/null
check "load slot 1: the strip's white balance and keep_pure come back" "[ \"\$(curl -s $S/strip | js 'd[\"settings\"][\"wb_g\"],d[\"settings\"][\"wb_b\"],d[\"settings\"][\"wb_keep_pure\"]')\" = '0.81 0.5 True' ]"
check "refused: slot 4 (422), empty slot (422), name with < (422), unknown action (422), not JSON (400)" "[ \$(code '{\"slot\":4,\"action\":\"save\"}') = 422 ] && [ \$(code '{\"slot\":3,\"action\":\"load\"}') = 422 ] && [ \$(code '{\"slot\":1,\"action\":\"save\",\"name\":\"a<b\"}') = 422 ] && [ \$(code '{\"slot\":1,\"action\":\"x\"}') = 422 ] && [ \$(code 'nope') = 400 ]"
check "a 25-character name is refused, 24 kept" "[ \$(code '{\"slot\":3,\"action\":\"save\",\"name\":\"abcdefghijklmnopqrstuvwxy\"}') = 422 ] && [ \$(code '{\"slot\":3,\"action\":\"save\",\"name\":\"abcdefghijklmnopqrstuvwx\"}') = 200 ]"
curl -s -XPOST -d '{"slot":2,"action":"clear"}' $S/wb_slots > /dev/null
check "clear: slot 2 empty again" "[ \"\$(curl -s $S/wb_slots | js 'd[\"slots\"][1].get(\"empty\")')\" = True ]"
kill $SIMPID; wait $SIMPID 2>/dev/null
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8
check "slots kept after a restart" "[ \"\$(curl -s $S/wb_slots | js 'd[\"slots\"][0][\"name\"],d[\"slots\"][1].get(\"empty\"),d[\"slots\"][2][\"name\"]')\" = 'Βράδυ True abcdefghijklmnopqrstuvwx' ]"

# ---- page
P="$D/en.html"; curl -s $H/ambi/en > "$P"
check "page: three slot rows (name, Save, Load, clear) in the camera card" "[ \$(grep -o 'class=\"wb-slot\" data-n=' $P | wc -l) = 3 ] && grep -q 'class=\"wb-slot-load\"' $P && python3 -c \"import sys;p=open('$P').read();sys.exit(0 if p.index('id=\\\"espCamPanel\\\"')<p.index('id=\\\"wbSlots\\\"') else 1)\""
check "page: camera steps once per new frame, at most 40 steps" "grep -q 'v.currentTime===cam.vt' $P && grep -q '++cam.steps>40' $P"
check "page: state polling gives up after 3 s (AbortController)" "grep -q 'new AbortController(),tm=setTimeout(()=>ac.abort(),3000)' $P"
check "page: Diagnosis compares WLED with wled_ma_target" "grep -q 'wled_ma_target' $P"

kill $SIMPID 2>/dev/null; wait $SIMPID 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
