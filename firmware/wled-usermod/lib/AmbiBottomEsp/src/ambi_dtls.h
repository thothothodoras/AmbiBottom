// DTLS-PSK server for the Hue Entertainment stream on the ESP32 (UDP 2100).
// Same job and same rules as the Linux bridge's src/dtls_psk_server.cpp
// (one client at a time, HelloVerifyRequest cookie, PSK looked up by the Hue
// username, end the open session at once when asked), written against plain
// BSD sockets + mbedTLS 2.28 (arduino-esp32 2.0.x) or 3.x (Linux), with its
// own UDP glue and timer so it needs neither MBEDTLS_NET_C nor
// MBEDTLS_TIMING_C (both are off in the ESP32 SDK build).
//
// Because nothing in it is ESP-specific, the Linux test suite runs the very
// same file against a real DTLS client (tests/test_dtls_loopback.cpp built a
// second time, ctest name esp_dtls).
//
// Not ported from the Linux server: keeping a session across a restart
// (mbedtls_ssl_context_save) - CONFIG_MBEDTLS_SSL_CONTEXT_SERIALIZATION is
// off in the stock arduino-esp32 SDK.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ambi_dtls {

// v0.37: counters for the page (device diagnostics). Seen on the device 2026-11-18:
// WLED's loop stopped being run while this server's task (higher priority) was
// up; these say whether the accept loop was spinning.
struct Stats {
    unsigned long accept_turns = 0;   // accept-loop iterations
    unsigned long select_errors = 0;  // select() < 0 in the accept loop
    unsigned long peek_errors = 0;    // recvfrom(MSG_PEEK) < 0 in the accept loop
    int fd = -1;
};
Stats stats();

using PskLookup = std::function<std::vector<uint8_t>(const std::string& identity)>;
using FrameHandler = std::function<void(const uint8_t* data, size_t len)>;

struct Options {
    uint16_t port = 2100;
    double idle_timeout_s = 5.0;
    // The TV waits 3 s for the handshake (see shared/ambi_core/tv_lock.hpp);
    // an abandoned one must not block the next for much longer.
    double handshake_timeout_s = 6.0;
    // Both optional. *stop: leave run_server(). *drop_session: end the open
    // session now (set on stream.active=false); cleared here.
    const std::atomic<bool>* stop = nullptr;
    std::atomic<bool>* drop_session = nullptr;
    // ok, peer ip, error text when it failed. Not called for the cookie round
    // or for stray packets of an old session.
    std::function<void(bool ok, const std::string& ip, const std::string& detail)> on_handshake;
    // A client keeps sending records of a session we do not have (we were
    // restarted while it streamed). active=false when a new handshake works.
    std::function<void(const std::string& ip, uint64_t packets, bool active)> on_old_session;
    std::function<void(bool in_session)> on_session;
};

// Blocking; run it on its own thread / FreeRTOS task. Returns on a fatal
// setup error or when *stop becomes true.
void run_server(const Options& opts, const PskLookup& psk_lookup, const FrameHandler& on_frame);

}  // namespace ambi_dtls
