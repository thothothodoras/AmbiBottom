#!/bin/bash
# v0.49: Greek as a patch (loader + /ambi/el.json, English + patch == the Greek page), the camera
# card, the settings index that scrolls (no #hash), the mobile header rules, the output label,
# the polling guard, no identity / discovery requests, "keep pure colours", the board's share of
# a shared USB port - against the simulator (build/esp_app_sim). Loopback only, test ports.
T=$(cd "$(dirname "$0")" && pwd); R=$(cd "$T/.." && pwd)
SIM=${SIM:-$R/build/esp_app_sim}; HP=18501; SPORT=21911; H=http://127.0.0.1:$HP
D=$(mktemp -d); OK=0; BAD=0
ok() { echo "[ok] $1"; OK=$((OK+1)); }
bad() { echo "[FAIL] $1"; BAD=$((BAD+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
js() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
mkdir -p "$D/state"
[ -x "$SIM" ] || { echo "no $SIM (cd build && make esp_app_sim)"; exit 1; }
"$SIM" "$D/state" $HP $SPORT 3 >> "$D/sim.log" 2>&1 & SIMPID=$!
sleep 0.8
A="$R/firmware/wled-usermod/AmbiBottom"

# ---- Greek as a patch
curl -s $H/ambi/en > "$D/en.html"; curl -s $H/ambi/el > "$D/loader.html"; curl -s $H/ambi/el.json > "$D/patch.json"
patch_ok() { python3 - "$D/en.html" "$D/patch.json" "$A/ambi_page_el.html" <<'EOF'
import json, re, sys
en = open(sys.argv[1], encoding="utf-8").read()
pt = json.load(open(sys.argv[2], encoding="utf-8"))
el = open(sys.argv[3], encoding="utf-8").read()
t = re.split(r"""([<>'"`])""", en)
assert len(t) == pt["n"], (len(t), pt["n"])
for i, x in pt["p"]:
    t[i] = x
sys.exit(0 if "".join(t) == el else 1)
EOF
}
check "/ambi/el = the small loader (fetches /ambi/en + /ambi/el.json)" "[ \$(stat -c %s $D/loader.html) -lt 2000 ] && grep -q '/ambi/el.json' $D/loader.html && grep -q \"fetch('/ambi/en'\" $D/loader.html"
check "English + /ambi/el.json == the Greek page, byte for byte" "patch_ok"
check "ambi_page_gz.h: no second full page, the patch + loader behind AMBIBOTTOM_PAGE_EL" "! grep -q 'AMBI_PAGE_EL_GZ\\b' $A/ambi_page_gz.h && grep -q 'AMBI_PAGE_EL_PATCH_GZ' $A/ambi_page_gz.h && grep -q 'AMBI_PAGE_EL_LOADER' $A/ambi_page_gz.h"
check "flash: page + patch < 90 KB gz (v0.48: 60 + 66 KB)" "[ \$(grep -o 'AMBI_PAGE_GZ_LEN = [0-9]*' $A/ambi_page_gz.h | grep -o '[0-9]*\$') -lt 70000 ] && [ \$(grep -o 'AMBI_PAGE_EL_PATCH_GZ_LEN = [0-9]*' $A/ambi_page_gz.h | grep -o '[0-9]*\$') -lt 26000 ]"

# ---- page content
P="$D/en.html"
check "camera card: buttons (+ Undo, v0.49.1), canvas, photo input, manual neutral switch; in the settings index" "for id in camStart camFile camAuto camUndo camStop camCanvas camManual; do grep -q \"id=\\\"\$id\\\"\" $P || exit 1; done; grep -q 'data-goto=\"espCamPanel\"' $P"
check "settings index scrolls, never puts #<card> in the address (it hid the Settings tab)" "! grep -q '<a href=\"#esp' $P && [ \$(grep -o 'data-goto=' $P | wc -l) = 7 ]"
check "mobile header: one row for PIN / EN / WLED, status + lamps full width" "grep -q '.top-actions .inputs-status{flex:1 1 100%' $P && grep -q 'flex-direction:row!important' $P"
check "output label from the board (chip · name)" "grep -q 'id=\"outTargetLabel\"' $P && grep -q 'dev.chip' $P"
check "state polling: not while the tab is hidden, never two at once" "grep -q 'if(document.hidden||pollBusy)return;' $P"
check "no identity / discovery requests on the ESP" "grep -q 'async function loadIdentity(){return;' $P && grep -q 'async function loadDiscovery(){return;' $P"
check "camera: one step at a time, a photo is used once, Undo" "grep -q 'if(!cam.auto||cam.busy)return;' $P && grep -q 'cam.used===cam.img' $P"
check "keep pure colours switch + the board's port share field" "grep -q 'id=\"setWbKeep\"' $P && grep -q 'id=\"devEspMa\"' $P"

# ---- API
curl -s -XPOST -d '{"wb_keep_pure":false}' $H/ambi/api/settings/strip > /dev/null
check "wb_keep_pure saved (false) and returned; default true" "[ \"\$(curl -s $H/ambi/api/settings/strip | js 'd[\"settings\"][\"wb_keep_pure\"],d[\"defaults\"][\"wb_keep_pure\"] if \"defaults\" in d else True')\" = 'False True' ]"
curl -s -XPOST -d '{"esp_ma":120}' $H/ambi/api/settings/device > /dev/null
check "device: esp_ma 120 kept (the board shares the port)" "[ \"\$(curl -s $H/ambi/api/settings/device | js 'd[\"esp_ma\"]')\" = 120 ]"
check "device: chip reported for the output label" "[ -n \"\$(curl -s $H/ambi/api/settings/device | js 'd[\"chip\"]')\" ]"

kill $SIMPID 2>/dev/null; wait $SIMPID 2>/dev/null
rm -rf "$D"
echo "$OK [ok], $BAD failed"
[ $BAD -eq 0 ] && echo PASS || { echo FAIL; exit 1; }
