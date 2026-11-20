// Hue v1 REST logic of the fake bridge, with no sockets, no files and no
// threads in it: one request in, one response out. v0.32: taken out of
// src/hue_rest.cpp unchanged so that the Linux bridge (hue_rest.cpp, POSIX
// sockets) and the ESP32 usermod (firmware/wled-usermod, AsyncWebServer) run
// the SAME code - group ids from 1, reuse by type + lights, placeholder for
// ids that are gone, real DELETE, error 1 for unknown users, the lost-pairing
// log (all v0.31, docs/TV-RE-FINDINGS.md section B).
//
// Needs cJSON (Linux: libcjson; ESP-IDF/arduino-esp32: the bundled "json"
// component) and the C++17 standard library. Not thread safe: the caller
// serialises calls to handle().
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bridge_identity.hpp"

// Where the log lines go. Default: stderr (the Linux bridge's log pipe; on the
// ESP32 that is the serial console).
#ifndef AMBI_LOG
#include <cstdio>
#define AMBI_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif

namespace hue_core {

struct Response {
    int status = 404;
    std::string content_type = "application/json";
    std::string body;
};

struct PairedUser {
    std::string clientkey_hex;  // 32 hex digits, or empty when none was asked for
};

// The pairing store as the REST logic needs it. Linux: an adapter around
// pairing_store::ThreadSafeStore. ESP32: NVS.
class Pairing {
public:
    virtual ~Pairing() = default;
    virtual bool get(const std::string& username) = 0;  // true = a user we issued
    virtual std::pair<std::string, PairedUser> pair(const std::string& devicetype, bool generate_clientkey) = 0;
    virtual std::vector<std::pair<std::string, std::string>> users_with_devicetype() = 0;
    virtual long link_remaining_ms() = 0;  // > 0 while the link button window is open
    virtual void note_pair_refused() = 0;
};

struct Options {
    std::vector<std::string> lamp_names = {"Bottom-Left", "Bottom-Middle", "Bottom-Right"};
    std::string apiversion = "1.67.0";
    std::string swversion = "1967054020";
    std::string tv_ip;                 // empty = pairing from any address
    bool require_link_button = false;
    // Group store as one JSON text. load: "" when there is none. save: called
    // after every change. Both optional (no persistence).
    std::function<std::string()> load_groups_text;
    std::function<void(const std::string&)> save_groups_text;
    std::function<bool(std::string& group_id, std::string& owner)> take_resumed_stream;
    std::function<void()> on_lightgroup_used;
    std::function<void(const std::string& username, const std::string& group_id, bool active,
                       const std::vector<std::string>& lamp_ids)> on_stream_active;
    std::function<void(const std::string& src_ip, const std::string& user_agent)> on_description;
    // v0.37: "the TV streams from an OLD DTLS session" (we restarted while it streamed; it never
    // notices over UDP and needed Ambilight+hue off/on). The TV's stream thread checks the bridge
    // every 3 s with GET /api/<user>/lights/1 and, after more than 3 failures, goes to its
    // Reconnection state (org.droidtv.ambihue PostALState.LampUpdateThread + Constants.mStates:
    // PostLamp + EvPostException -> StateRecon), which finds the bridge again and starts a NEW
    // stream. When this returns true for the asking address, GET .../lights/<id> answers 503.
    std::function<bool(const std::string& src_ip)> fail_lamp_check;
};

class Bridge {
public:
    Bridge(Options options, const bridge_identity::Identity& identity, Pairing& pairing);
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // method "GET" / "PUT" / "POST" / "DELETE"; path as received (query and
    // trailing slashes are removed here); body may be empty.
    Response handle(const std::string& method, const std::string& path, const std::string& body,
                    const std::string& src_ip, const std::string& user_agent);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace hue_core
