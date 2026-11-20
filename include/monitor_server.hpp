// This bridge's own small web dashboard - NOT part of the Hue protocol
// (same spirit as hue_rest's /ambi/settings extension, just on its own
// port so a buggy/probed request here can never touch the TV-facing REST
// or DTLS servers). Shows the live lamp colours, bridge identity and
// pairing status, and has Stop/Restart buttons for the systemd service
// (see systemd/ambi-bridge.service) so the page itself answers the
// earlier "I want a button that starts the server" request as far as
// this architecture allows: a page served BY the process obviously can't
// start that same process from cold, so there is deliberately no "Start"
// button here - only Stop/Restart. Starting from cold is systemd's job,
// via `systemctl enable ambi-bridge` so it comes up on boot.
//
// v0.28: settings and the Start/Stop/Restart/Exit controls need this Linux
// machine's own browser or a login (login_store.hpp). Before that: anyone
// who can reach this port on your LAN could stop/restart the bridge
// from here, same trust model as already being able to repair/reconfigure
// it via the Hue REST API with no auth either. Fine for a private home
// LAN; not something to expose past it.
#pragma once
#include <functional>
#include "discovery.hpp"
#include <cstdint>
#include <string>

#include "bridge_identity.hpp"
#include "jointspace_client.hpp"
#include "monitor_state.hpp"
#include "pairing_store.hpp"
#include "settings.hpp"
#include "identity_store.hpp"
#include "strip_stats.hpp"
#include "login_store.hpp"

namespace monitor_server {

struct Options {
    uint16_t port = 8098;
    // systemd unit name to control via "systemctl <action> <unit>" from
    // the page's Stop/Restart buttons, e.g. "ambi-bridge.service". Empty
    // disables those buttons (the page still shows live status) - running
    // `systemctl` against an empty/guessed unit name would be worse than
    // just not offering the control at all.
    std::string systemd_unit;
    // v9: whether hue_rest requires the virtual link button for pairing
    // (--pairing button). Only changes what the page shows; the "Pair"
    // button itself (POST /api/pair) always opens the window.
    bool pairing_requires_button = false;
    // v0.24: shown in the page footer and /api/state "bridge_version".
    std::string version;
    // v0.24 (PLAN item 4): restart this process (same arguments, newest
    // build) - for the page's Restart button when there is no systemd unit.
    // Returns "" when the restart is under way, else why not. Empty = off.
    std::function<std::string(const std::string& who)> request_restart;
    // v0.28: the in-place service controls (no systemd unit): action is
    // "start" | "stop" | "restart" | "exit"; returns "" when under way.
    std::function<std::string(const std::string& action, const std::string& who)> request_service;
    // v0.28: "running" | "stopped" (shown on the page; empty function = running).
    std::function<std::string()> service_state;
    // v0.28: one user + password for edits from other devices (nullptr = off,
    // edits only from this Linux machine's browser as before).
    login_store::Store* login = nullptr;
};

// Blocking call: runs its own accept loop forever, on its own thread.
// `js_settings` backs GET/POST /api/settings/jointspace - the page's own
// "Ρυθμίσεις σύνδεσης TV" form - which is loopback-only (same security
// model as the old Python prototype): the POST is only honoured when the
// request's Host/Origin and peer address all say this is the Linux
// machine's own browser talking to its own bridge, never a LAN client.
void run_server(const Options& opts, const bridge_identity::Identity& identity,
                 pairing_store::ThreadSafeStore& pairing, monitor_state::SharedState& state,
                 jointspace::Settings& js_settings, settings::SharedSettings* strip_settings = nullptr,
                 identity_store::Shared* identities = nullptr, discovery::Registry* discovery = nullptr,
                 strip_stats::Stats* strip_stats = nullptr);
// `strip_stats` (v0.23), when non-null, backs GET /api/strip_stats (the
// per-LED log and the limiter's compromise episodes) and POST
// /api/strip_stats/reset (clears the "since reset" totals; local browser only).
// `discovery` (v0.19), when non-null, backs GET/POST /api/discovery - the
// page's "Ποιος ρωτάει" card (who searched, full User-Agent) and the
// "answer only Hue searches" switch (changes only from the local browser).
// `identities` (v0.17), when non-null, backs GET/POST /api/identity - the
// page's "Ταυτότητα bridge" card (name, MAC, lamp count); changes only from
// the local browser, applied at the next start.
// `strip_settings` (v0.15, Phase 3), when non-null, backs GET/POST
// /api/settings/strip - the page's "Ταινία LED & ρεύμα" form, with the same
// local-browser-only rule for changes as the JointSpace form.

}  // namespace monitor_server
