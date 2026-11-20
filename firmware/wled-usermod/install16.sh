#!/bin/bash
# install16.sh (v0.36) - set up a WLED 16.0.x checkout to build AmbiBottom for the AtomS3 Lite.
# Writes platformio_override.ini (git-ignored by WLED) and, v0.46, adds an "Ambi" button to WLED's
# top bar (wled00/data/index.htm; WLED's build turns it into html_ui.h). Run again after updating WLED;
# if WLED's top bar changed, the patch stops with a message instead of guessing.
# usage: firmware/wled-usermod/install16.sh /path/to/WLED   then   cd /path/to/WLED && pio run -e atoms3_ambibottom
set -e
WLED=${1:?usage: install16.sh /path/to/WLED}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
grep -q 'REGISTER_USERMOD' "$WLED/wled00/fcn_declare.h" || { echo "this WLED has no REGISTER_USERMOD (0.15?): use install.sh"; exit 1; }
O="$WLED/platformio_override.ini"
[ -f "$O" ] && ! grep -q 'written by install16.sh' "$O" && { echo "$O exists and is not ours: not overwritten"; exit 1; }
sed "s|@REPO@|$REPO|g" "$REPO/firmware/wled-usermod/platformio_override.wled16.ini" > "$O"
echo "wrote $O (env atoms3_ambibottom)"
# v0.46: "Ambi" button in WLED's top bar, after "Peek" (idempotent; marker: id="buttonAmbi")
I="$WLED/wled00/data/index.htm"
python3 - "$I" <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
if 'id="buttonAmbi"' in s:
    print("index.htm: Ambi button already there"); sys.exit(0)
anchor = '<button id="buttonSr" onclick="toggleLiveview()"><i class="icons">&#xe410;</i><p class="tab-label">Peek</p></button>'
if s.count(anchor) != 1:
    sys.exit("index.htm: WLED's top bar changed (no Peek button found) - Ambi button NOT added; update install16.sh")
btn = ('<button id="buttonAmbi" onclick="window.location.href=getURL(\'/ambi\');" title="AmbiBottom page">'
       '<i class="icons"><svg width="24" height="24" viewBox="0 0 24 20" fill="none" stroke="currentColor" stroke-width="2" '
       'stroke-linecap="round" aria-hidden="true" style="vertical-align:middle"><rect x="4" y="3" width="16" height="11" rx="1.5"/>'
       '<path d="M9 18h6M1 6v5M23 6v5" opacity=".7"/></svg></i><p class="tab-label">Ambi</p></button>')
open(p, "w", encoding="utf-8").write(s.replace(anchor, anchor + "\n\t\t\t" + btn, 1))
print("index.htm: Ambi button added (WLED rebuilds its UI on the next pio run)")
EOF

