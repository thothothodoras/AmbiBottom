#!/bin/bash
# Adds the AmbiBottom usermod to a WLED 0.15.x source tree:
#  - registers it in wled00/usermods_list.cpp (0.15 has no other way), and
#  - writes platformio_override.ini with the build environment esp32s3dev_8MB
#    (AtomS3 Lite: ESP32-S3, 8 MB flash, no PSRAM; LED data on GPIO5).
# Nothing is copied: the build reads the usermod and its two libraries from
# this repository (symlink:// lib_deps), so there is one source.
#
# usage: firmware/wled-usermod/install.sh /path/to/WLED    (run again any time; it changes nothing twice)
set -e
WLED=${1:?usage: install.sh /path/to/WLED}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
L="$WLED/wled00/usermods_list.cpp"
[ -f "$L" ] || { echo "not a WLED 0.15 tree: $L is missing"; exit 1; }
grep -q 'void registerUsermods()' "$L" || { echo "unexpected usermods_list.cpp (WLED 0.16+ registers usermods differently)"; exit 1; }
if ! grep -q 'USERMOD_AMBIBOTTOM' "$L"; then
  python3 - "$L" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
i = s.index('void registerUsermods()')
s = s[:i] + '#ifdef USERMOD_AMBIBOTTOM\n  #include "usermod_ambibottom.h"\n#endif\n\n' + s[i:]
j = s.rindex('}')
s = s[:j] + '\n  #ifdef USERMOD_AMBIBOTTOM\n  UsermodManager::add(new AmbiBottomUsermod());\n  #endif\n' + s[j:]
open(p, 'w').write(s)
PY
  echo "registered in $L"
else
  echo "already registered in $L"
fi
O="$WLED/platformio_override.ini"
if [ -f "$O" ] && ! grep -q 'AmbiBottom' "$O"; then
  echo "$O exists and is not ours: add the [env:esp32s3dev_8MB] section from $HERE/platformio_override.sample.ini by hand"
else
  sed "s#@REPO@#$REPO#g" "$HERE/platformio_override.sample.ini" > "$O"
  echo "wrote $O"
fi
echo "build:  cd $WLED && pio run -e esp32s3dev_8MB"
