// Minimal Hue v1 CLIP REST server - just enough surface for the TV to
// discover this bridge, pair with it, see an Entertainment-capable light
// group, and flip stream.active on/off. Mirrors fakebridge.py's Handler
// class (routes, response shapes, the "3 lamps: Bottom-Left/Middle/Right"
// default setup) since that version is the one already validated against
// the user's real Philips 65OLED707.
//
// Deliberately plain POSIX sockets, one request per connection
// ("Connection: close" on every response) - simplicity over keep-alive
// performance, since REST traffic here is low-rate (pairing, occasional
// light/group PUTs); the actual colour path is the separate DTLS-PSK
// stream, not this server. This file is LXC-prototype-only scaffolding: it
// does not need to port to the ESP32/WLED usermod, which will plug into
// WLED's own existing HTTP stack instead.
#pragma once
#include "discovery.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "bridge_identity.hpp"
#include "pairing_store.hpp"
#include "settings.hpp"

namespace hue_rest {

struct Options {
    uint16_t port = 80;
    // Standard Ambilight+hue wizard setup: 3 logical lamp positions along
    // the bottom edge. This is independent of the LED strip's physical
    // LED_COUNT - the colour pipeline resamples from however many lamps
    // the TV streams onto however many LEDs settings.led_count specifies.
    std::vector<std::string> lamp_names = {"Bottom-Left", "Bottom-Middle", "Bottom-Right"};
    // apiversion/swversion >= 1.22 is what makes the TV treat this bridge
    // as "streaming capable" (HueControl.CheckBridgeHueStreamSupport) and
    // switch its wizard to the Entertainment path, per fakebridge.py's own
    // comment and the values it uses for --stream mode.
    std::string apiversion = "1.67.0";
    std::string swversion = "1967054020";
    // Pairing is normally restricted to requests whose source IP matches
    // this (the TV's IP) - set empty to allow pairing from any source,
    // which is what "the ESP does its own fresh pairing" implies for v1
    // until the config page's IP-lock option is wired up.
    std::string tv_ip;
    // v9: when true, POST /api only pairs while the virtual link button's
    // window is open (pairing_store::ThreadSafeStore::press_link_button),
    // otherwise error 101 - the real-bridge behaviour. main.cpp turns this
    // on by default (--pairing button); false keeps the old auto-accept
    // behaviour (--pairing auto, and the unit tests).
    bool require_link_button = false;
    // Where to persist created groups (Entertainment group id, lights,
    // locations) between restarts, e.g. "<state-dir>/groups.json". Empty
    // disables persistence (groups reset to the default on every restart -
    // fine for tests, but means the TV won't auto-resume streaming after a
    // bridge restart without the user re-running "Configure" on the TV).
    // stream.active/owner are deliberately never persisted - a restart
    // always comes back with streaming off, and the TV re-requests it.
    std::string groups_store_path;
    // v0.29: after a DTLS session continued across a restart (dtls_psk
    // session_file), the TV does not send stream.active again. Checked before
    // every request: returns true once with the group + owner to mark active.
    std::function<bool(std::string& group_id, std::string& owner)> take_resumed_stream;
    // v0.31: the TV created or was answered a LightGroup (it is not on the
    // Entertainment path). Optional; main.cpp passes it to the monitor.
    std::function<void()> on_lightgroup_used;
    // v0.37: see hue_core::Options::fail_lamp_check (old DTLS session -> make the TV reconnect)
    std::function<bool(const std::string& src_ip)> fail_lamp_check;
};

// Fired whenever a group's stream.active changes. v1's DTLS server already
// listens continuously regardless of this flag (unlike fakebridge.py's
// openssl-subprocess approach, which has to be started/stopped); this
// callback exists so main.cpp can log it and, later, scope rendering to
// just the lamp ids actually in the active streaming group.
using StreamActiveHandler = std::function<void(const std::string& username,
                                                 const std::string& group_id,
                                                 bool active,
                                                 const std::vector<std::string>& lamp_ids)>;

// Blocking call: runs the accept loop forever on its own thread. `identity`
// is used read-only (filled in once at startup by main.cpp, once the LAN IP
// is known). `pairing` is shared with the DTLS server's PSK lookup, so it
// is pairing_store::ThreadSafeStore - every mutation here is saved to
// `pairing_store_path` immediately (same atomic-write safety as settings).
// `strip_settings`, when non-null, adds GET/PUT /ambi/settings - the
// runtime-settable LED_COUNT/diffuser/current_budget_ma/etc config the
// user asked to be a page/API setting rather than a compile-time constant.
// This isn't part of the real Hue API; it's this bridge's own small
// extension, namespaced under /ambi/ to keep clear of the CLIP v1 routes.
void run_server(const Options& opts,
                 const bridge_identity::Identity& identity,
                 pairing_store::ThreadSafeStore& pairing,
                 const std::string& pairing_store_path,
                 const StreamActiveHandler& on_stream_active,
                 settings::SharedSettings* strip_settings = nullptr,
                 discovery::Registry* discovery = nullptr);
// `discovery` (v0.19), when non-null, is told about every GET
// /description.xml (IP + User-Agent) for the monitor's "Ποιος ρωτάει" card.

}  // namespace hue_rest
