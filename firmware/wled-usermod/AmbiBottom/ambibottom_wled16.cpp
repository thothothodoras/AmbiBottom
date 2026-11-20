// WLED 16 registration of the AmbiBottom usermod (v0.36).
// WLED 16 replaced 0.15's wled00/usermods_list.cpp with REGISTER_USERMOD(): every
// library listed in an environment's custom_usermods that defines one is linked in
// (the library must not be archived: "libArchive": false in library.json). The usermod
// itself is the same header the 0.15 build uses (usermod_ambibottom.h).
#include "wled.h"
#include "usermod_ambibottom.h"

static AmbiBottomUsermod ambibottom;
REGISTER_USERMOD(ambibottom);
