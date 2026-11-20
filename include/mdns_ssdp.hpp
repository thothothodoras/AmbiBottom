// Bridge discovery: mDNS (_hue._tcp) and SSDP, mirroring fakebridge.py's
// start_mdns()/ssdp_thread() so the TV can find this bridge the same way it
// finds the user's already-working Python version.
//
// LXC-prototype-only scaffolding, like hue_rest.hpp - the ESP32/WLED
// usermod will use its own existing mDNS stack (ESP-IDF's mdns component),
// not this file.
//
// mDNS is done by shelling out to `avahi-publish-service`/
// `avahi-publish-address` (confirmed present and working for this exact
// invocation shape via `avahi-publish-service --help` and a live
// publish+avahi-browse round trip in the sandbox this was built in - see
// README for what that did and didn't prove). SSDP is a small from-scratch
// UDP responder using plain POSIX sockets, since that needs no external
// dependency and is simple enough to just get right directly.
#pragma once
#include "bridge_identity.hpp"
#include "discovery.hpp"

namespace mdns_ssdp {

// Spawns `avahi-publish-address` + `avahi-publish-service` as background
// child processes (best-effort: logs a warning and continues without mDNS
// if the binaries aren't installed, same graceful-degradation policy as
// fakebridge.py's "zeroconf not installed -> mDNS disabled" path). Does
// not block. Before spawning, also makes a best-effort attempt to kill any
// stale avahi-publish-* processes left over from a previous *unclean* exit
// of this same program (SIGKILL, crash, power loss) that are still holding
// our exact hostname - see stop_mdns() below for the normal-exit path.
void start_mdns(const bridge_identity::Identity& identity);

// Call once, at shutdown, after start_mdns() was called earlier in this
// same process. Sends SIGTERM to the avahi-publish-address/-service child
// processes spawned by start_mdns() and waits (briefly) for them to exit.
// avahi-publish-* are designed to withdraw their record and exit cleanly
// on SIGTERM - this is the normal way to stop them, not an afterthought -
// so calling this on every graceful shutdown (SIGINT/SIGTERM, see main.cpp)
// means the *.local hostname is actually free again immediately, instead
// of lingering until something else collides with it on the next start.
// A no-op if start_mdns() was never called or found no avahi binary.
void stop_mdns();

// Blocking call: runs the SSDP UDP responder (bind :1900, multicast join,
// M-SEARCH replies, periodic NOTIFY) forever. Intended for its own thread.
// Logs and returns early only if it can't bind the port at all.
// v0.19: `discovery`, when non-null, is told about every M-SEARCH and
// decides (hue_only switch) which searches are answered; null = answer all.
void run_ssdp_responder(const bridge_identity::Identity& identity, discovery::Registry* discovery = nullptr);

}  // namespace mdns_ssdp
