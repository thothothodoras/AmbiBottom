// DTLS-PSK server for the Hue Entertainment stream (UDP, one client at a
// time - only the TV ever connects). Built on mbedTLS (not OpenSSL) because
// that is what ESP-IDF already bundles; this file deliberately uses only
// portable mbedTLS APIs (no POSIX-specific calls beyond mbedtls_net_*,
// which has direct ESP-IDF/lwIP equivalents) so the accept/handshake/read
// logic can move into the WLED usermod later with the socket layer swapped,
// not rewritten.
//
// Pattern follows mbedTLS's own reference DTLS server example
// (programs/ssl/dtls_server.c): bind once, mbedtls_net_accept() per client
// (which internally "connects" a fresh UDP socket to that peer after its
// first datagram), HelloVerifyRequest cookie round-trip against DoS, then
// handshake, then a read loop with an idle timeout that ends the session.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dtls_psk {

// Looks up the PSK (raw bytes, NOT hex) for a given PSK identity (the Hue
// "username"). Return empty vector if the identity is unknown - the
// handshake will then fail, exactly like a real Hue bridge rejecting an
// unpaired client.
using PskLookup = std::function<std::vector<uint8_t>(const std::string& identity)>;

// Called once per successfully decrypted application-data record (one
// HueStream frame, usually - DTLS preserves datagram boundaries so no
// re-framing is needed here, unlike the openssl-s_server byte-pipe
// approach in fakebridge.py).
using FrameHandler = std::function<void(const uint8_t* data, size_t len)>;

// v0.21: a client (the TV) keeps sending encrypted stream records from a
// session this process never had - the bridge was restarted while the TV
// was streaming, and the TV doesn't notice (UDP). Called with active=true
// when that starts and every ~2 s while it goes on (packets = how many so
// far), and active=false once a new handshake from that client succeeds.
using OldSessionHandler = std::function<void(const std::string& ip, uint64_t packets, bool active)>;

struct Options {
    OldSessionHandler on_old_session;  // optional
    // v0.24 (review item 5): when *stop becomes true during a session, the
    // read loop ends it with close_notify (so the client knows the session
    // is over) and *in_session goes false. Both optional.
    const std::atomic<bool>* stop = nullptr;
    std::atomic<bool>* in_session = nullptr;
    // v0.29: keep the TV's session across stop/start/restart/exit. When
    // non-empty: on stop, a live session is saved here (mbedtls_ssl_context_save,
    // file mode 0600, holds the session keys) INSTEAD of close_notify; on
    // start, a saved session younger than resume_max_age_s is read and the
    // file deleted, and the first datagrams from that same TV ip:port continue
    // it (mbedtls_ssl_context_load) - no new handshake, nothing to do on the TV.
    std::string session_file;
    // v0.29: called once when the first frame of a continued (saved) session
    // is decrypted - main.cpp then restores stream.active (optional).
    std::function<void()> on_resumed;
    double resume_max_age_s = 24 * 3600.0;
    // v0.31: the TV gives a new handshake only 3 s after stream.active=true
    // (PostALState.LampUpdateThread) and never retries in that process if it
    // fails. This server handles one client at a time, so a session that is
    // still open (waiting for its idle timeout) must not hold up the next
    // ClientHello. When *drop_session becomes true the read loop ends the
    // current session at once (the flag is cleared) - REST sets it on every
    // stream.active change. Optional.
    std::atomic<bool>* drop_session = nullptr;
    // v0.31: told about every finished handshake attempt that was a real
    // attempt (not the cookie round, not old-session packets): ok, peer ip,
    // and the error text when it failed. Optional.
    std::function<void(bool ok, const std::string& ip, const std::string& detail)> on_handshake;
    uint16_t port = 2100;
    double idle_timeout_s = 5.0;       // matches ha-hue-entertainment's FRAME_TIMEOUT
    // v0.31: was 30 s. A handshake the TV abandoned must not block the next
    // one for longer than the TV itself waits (3 s) plus a margin.
    double handshake_timeout_s = 6.0;
};

// Blocking call: runs the accept/handshake/read loop forever (intended to
// run on its own thread). Logs progress via stderr. Returns only on a fatal
// setup error (e.g. cannot bind the port); per-client errors are logged and
// the loop goes back to accepting the next client.
void run_server(const Options& opts, const PskLookup& psk_lookup, const FrameHandler& on_frame);

}  // namespace dtls_psk
