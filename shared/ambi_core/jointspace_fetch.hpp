// v0.37: the JointSpace client shared by the Linux bridge and the AmbiBottom
// usermod (ESP32). See src/jointspace_client.hpp for the rest (credentials
// file, polling loop: Linux only). No Linux- or ESP-only header here.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jointspace {

// One of the TV's processed Ambilight zones (0..255 per channel as the TV gives them).
struct Zone {
    uint8_t r = 0, g = 0, b = 0;
};

struct Sides {
    std::vector<Zone> left, top, right;
};

// Error codes, kept identical in spelling to the old bridge's on purpose -
// nothing here is user-facing prose, so the monitor page's JS (ported
// almost verbatim, see monitor_server.cpp's embedded page) already knows
// how to turn each one into a Greek status string.
// Persistent JointSpace client, same model as the old bridge's
// requests.Session: one kept-alive TLS connection reused across polls, and
// the TV's Digest nonce reused with an incrementing nonce count, so a
// steady-state poll is exactly one request on an already-open connection -
// no TLS handshake, no extra 401 round trip. Reconnects (once per fetch) if
// the TV closed the idle connection, and re-authenticates if the TV
// rotates its nonce. Not thread-safe; owned by the single polling thread.
//
// Until v6 this was a one-shot function that opened a brand-new TCP+TLS
// connection and did an unauthenticated 401 round trip on *every* poll,
// 5 polls/s at the default interval - and the old comment here claimed
// that was fine "at a sub-1 Hz poll rate", which the default never was.
// That handshake flood is what eventually made the real TV stop accepting
// connections on port 1926 until it was restarted.
//
// fetch() returns true and fills `out` on success; returns false and fills
// `error_code` (one of: invalid_address, network_timeout,
// network_unavailable, authentication_failed, forbidden, request_failed,
// invalid_response) on failure.
class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    bool fetch(const std::string& tv_ip, const std::string& username, const std::string& password, Sides& out,
               std::string& error_code, int timeout_ms = 4000);
    void reset();  // drop the connection and the cached challenge

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- v0.45: JointSpace pairing (the TV shows a PIN) ------------------------------------
// Protocol as implemented by ha-philipsjs (MIT, Home Assistant's Philips TV library):
//   1. POST /6/pair/request {"access":{"scope":["read","write","control"]},"device":{...}}
//      -> {"error_id":"SUCCESS","auth_key":"...","timestamp":N,"timeout":S}; the TV shows a PIN
//   2. POST /6/pair/grant with Digest login (device id / auth_key)
//      {"auth":{"auth_appId":"1","auth_timestamp":N,"auth_signature":SIG,"pin":"1234"},"device":{...}}
//      SIG = base64(HMAC-SHA1(shared key, "<N><pin>")) -> {"error_id":"SUCCESS"}
//   The credentials are then: username = the device id, password = auth_key.
// Error codes: the TV's error_id in lower case (e.g. "invalid_pin", "concurrent_pairing"), or
// network_*, invalid_address, request_failed, invalid_response.
struct PairState {
    std::string device_id;   // 32 hex digits, made up here (the "username" afterwards)
    std::string auth_key;    // from the TV (the "password" afterwards)
    std::string timestamp;   // from the TV, as digits
    std::string device_name;
    int timeout_s = 0;       // how long the TV waits for the PIN
};
bool pair_request(const std::string& tv_ip, const std::string& device_name, PairState& st, std::string& error_code,
                  int timeout_ms = 8000);
bool pair_grant(const std::string& tv_ip, const PairState& st, const std::string& pin, std::string& error_code,
                int timeout_ms = 8000);
std::string pair_signature(const std::string& timestamp, const std::string& pin);  // exposed for tests

// One-shot convenience wrapper (fresh Client per call) - fine for a single
// diagnostic request, never for polling.
bool fetch_processed_sides(const std::string& tv_ip, const std::string& username, const std::string& password,
                            Sides& out, std::string& error_code, int timeout_ms = 4000);

}  // namespace jointspace
