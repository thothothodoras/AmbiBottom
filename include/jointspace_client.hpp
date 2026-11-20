// Read-only client for the TV's own JointSpace API (the Philips Android TV
// local HTTPS API on port 1926), used only to show the TV's own left/top/
// right Ambilight screen-capture zones on the monitor dashboard - this is
// NOT part of the Hue bridge emulation and nothing here is ever written
// back to the TV. Ported from the old Python prototype's
// jointspace_transport.py / jointspace_settings.py (same endpoint, same
// HTTP Digest auth, same credential-storage security model), using
// mbedTLS instead of `requests` since this project has no HTTP library.
//
// Why this exists at all: the Hue Entertainment/DTLS stream this bridge
// emulates only ever carries the bottom 3 lamps' colours - it has no
// visibility into what the TV's own factory Ambilight is doing on the
// other three sides. JointSpace is the TV's own local API that exposes
// that, read-only, which is what lets the monitor page draw all four
// sides instead of just the bottom one.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "jointspace_fetch.hpp"  // v0.37: Zone, Sides, Client, fetch_processed_sides (shared with the ESP32)
#include "monitor_state.hpp"

namespace jointspace {

// Validated against a 65OLED707-shaped TV: processed_sides() below enforces
// 1..512 pixels per side and 0..255 int r/g/b, same bounds as the Python
// original - a malformed/hostile JSON body (wrong types, huge arrays,
// out-of-range values) is rejected rather than trusted.


// Local credential storage - mirrors jointspace_settings.py exactly:
// loaded once at startup from `path` (state-dir/jointspace_credentials.json,
// atomic temp-file+rename writes, 0600), status() never reveals the
// values, and update() is meant to be called only from a request already
// confirmed to be from the Linux machine's own browser (monitor_server.cpp
// enforces that - Host/Origin/127.0.0.1 - before ever calling update()).
class Settings {
public:
    // `tv_ip` is fixed for the process lifetime (from --tv-ip); only the
    // username/password are ever changed from the browser.
    Settings(std::string tv_ip, std::string initial_user, std::string initial_pass, std::string path);

    struct Status {
        bool available = false;    // a TV IP was configured at all (--tv-ip)
        bool configured = false;   // username+password are both non-empty
        bool persisted = false;    // a saved credentials file was loaded or written
        bool load_error = false;   // a credentials file existed but couldn't be read
    };
    Status status() const;

    // Validates (same rules as the Python original: both fields non-empty,
    // <=512 chars, no NUL/CR/LF), then writes atomically to `path`, then
    // takes effect immediately - the poll loop picks up the new revision
    // on its next cycle (see capture() below) and is woken early.
    // Returns false (nothing changed) on invalid input or a storage error.
    bool update(const std::string& username, const std::string& password, std::string& error_code);

    // Polling-thread side: snapshot of (revision, tv_ip, username,
    // password) to detect a credential change between poll cycles, and a
    // short wait that returns early if update() is called meanwhile.
    struct Capture {
        uint64_t revision;
        std::string tv_ip, username, password;
    };
    Capture capture() const;
    void wait_for_change_or(int delay_ms);

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::string tv_ip_;
    std::string user_, pass_;
    std::string path_;
    uint64_t revision_ = 0;
    bool persisted_ = false;
    bool load_error_ = false;
};

// Runs forever on its own thread (main.cpp detaches it), polling
// fetch_processed_sides() at `interval_ms` and reporting every result
// (success or failure) to `on_result` - matching main.cpp's existing
// callback-based wiring style for the other background loops.
using ResultCallback = std::function<void(bool ok, const std::string& status, const std::string& error,
                                           std::vector<monitor_state::Zone> left,
                                           std::vector<monitor_state::Zone> top,
                                           std::vector<monitor_state::Zone> right)>;
void run_poll_loop(Settings& settings, int interval_ms, const ResultCallback& on_result);

}  // namespace jointspace
