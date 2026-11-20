#include "ambi_dtls.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/version.h>

#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

#ifndef AMBI_LOG
#define AMBI_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif

// mbedtls/net_sockets.h is not used; these are its error values.
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif

namespace ambi_dtls {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string err_text(int ret) {
    char buf[160];
    buf[0] = 0;
    mbedtls_strerror(ret, buf, sizeof(buf));
    if (buf[0] == 0) std::snprintf(buf, sizeof(buf), "mbedTLS error -0x%04X", static_cast<unsigned>(-ret));
    return buf;
}

std::string ip_text(const sockaddr_in& a) {
    const uint32_t ip = ntohl(a.sin_addr.s_addr);
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 255u, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
    return buf;
}

bool same_peer(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// ---- UDP glue: one unconnected socket, one peer at a time ----------------
struct Bio {
    int fd = -1;
    sockaddr_in peer{};
    // A datagram from the same host but another port is waiting: the client
    // started over (the TV does after stream.active=false/true). It is left in
    // the socket for the accept step; the open session ends.
    bool new_client_waiting = false;
};

std::atomic<unsigned long> g_accept_turns{0}, g_select_errors{0}, g_peek_errors{0};
std::atomic<int> g_fd{-1};

// 1 = readable, 0 = timeout, <0 = error
int wait_readable(int fd, uint32_t timeout_ms) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000u);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
    return select(fd + 1, &rd, nullptr, nullptr, &tv);
}

int bio_send(void* ctx, const unsigned char* buf, size_t len) {
    auto* b = static_cast<Bio*>(ctx);
    const ssize_t n = sendto(b->fd, buf, len, 0, reinterpret_cast<const sockaddr*>(&b->peer), sizeof(b->peer));
    return n < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : static_cast<int>(n);
}

int bio_recv_timeout(void* ctx, unsigned char* buf, size_t len, uint32_t timeout_ms) {
    auto* b = static_cast<Bio*>(ctx);
    const auto start = Clock::now();
    const uint32_t budget = timeout_ms == 0 ? 250u : timeout_ms;
    for (;;) {
        const double used_ms = seconds_since(start) * 1000.0;
        if (used_ms >= budget) return MBEDTLS_ERR_SSL_TIMEOUT;
        const int r = wait_readable(b->fd, static_cast<uint32_t>(budget - used_ms));
        if (r == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
        if (r < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
        sockaddr_in src{};
        socklen_t sl = sizeof(src);
        unsigned char head[5] = {0, 0, 0, 0, 0};  // DTLS record: type, version (2), epoch (2)
        const ssize_t got = recvfrom(b->fd, head, sizeof(head), MSG_PEEK, reinterpret_cast<sockaddr*>(&src), &sl);
        if (got < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
        if (same_peer(src, b->peer)) {
            sl = sizeof(src);
            const ssize_t n = recvfrom(b->fd, buf, len, 0, reinterpret_cast<sockaddr*>(&src), &sl);
            return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : static_cast<int>(n);
        }
        // Same host, another port, and it is a handshake record of epoch 0 (a
        // ClientHello): the client started over. Anything else from another
        // port - e.g. stream records of an older session that is still being
        // sent - must not end the session that works.
        const bool client_hello = got >= 5 && head[0] == 22 && head[3] == 0 && head[4] == 0;
        if (src.sin_addr.s_addr == b->peer.sin_addr.s_addr && client_hello) {
            b->new_client_waiting = true;  // not consumed
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        // another host, or stray records, while a client is being served: drop it
        unsigned char sink[8];
        sl = sizeof(src);
        recvfrom(b->fd, sink, sizeof(sink), 0, reinterpret_cast<sockaddr*>(&src), &sl);
    }
}

// ---- timer callbacks (mbedtls_timing_* without MBEDTLS_TIMING_C) ----------
struct Timer {
    Clock::time_point start;
    uint32_t int_ms = 0, fin_ms = 0;
};

void timer_set(void* data, uint32_t int_ms, uint32_t fin_ms) {
    auto* t = static_cast<Timer*>(data);
    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
    t->start = Clock::now();
}

int timer_get(void* data) {
    auto* t = static_cast<Timer*>(data);
    if (t->fin_ms == 0) return -1;
    const double ms = seconds_since(t->start) * 1000.0;
    if (ms >= t->fin_ms) return 2;
    if (ms >= t->int_ms) return 1;
    return 0;
}

int psk_callback(void* p_psk, mbedtls_ssl_context* ssl, const unsigned char* identity, size_t identity_len) {
    const auto* lookup = static_cast<const PskLookup*>(p_psk);
    const std::string id(reinterpret_cast<const char*>(identity), identity_len);
    const std::vector<uint8_t> key = (*lookup)(id);
    if (key.empty()) return -1;
    return mbedtls_ssl_set_hs_psk(ssl, key.data(), key.size());
}

bool stopping(const Options& o) { return o.stop && o.stop->load(); }

}  // namespace

void run_server(const Options& opts, const PskLookup& psk_lookup, const FrameHandler& on_frame) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_cookie_ctx cookie_ctx;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_cookie_init(&cookie_ctx);

    Bio bio;
    auto cleanup = [&] {
        if (bio.fd >= 0) close(bio.fd);
        mbedtls_ssl_cookie_free(&cookie_ctx);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    };

    const char* pers = "ambi-bottom-dtls";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char*>(pers), std::strlen(pers));
    if (ret != 0) {
        AMBI_LOG("[dtls] ctr_drbg_seed failed: %s\n", err_text(ret).c_str());
        cleanup();
        return;
    }

    bio.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (bio.fd < 0) {
        AMBI_LOG("[dtls] socket() failed\n");
        cleanup();
        return;
    }
    {
        int yes = 1;
        setsockopt(bio.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(opts.port);
        if (bind(bio.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            AMBI_LOG("[dtls] bind udp/%u failed\n", static_cast<unsigned>(opts.port));
            cleanup();
            return;
        }
    }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0) ret = mbedtls_ssl_cookie_setup(&cookie_ctx, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        AMBI_LOG("[dtls] TLS setup failed: %s\n", err_text(ret).c_str());
        cleanup();
        return;
    }
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_dtls_cookies(&conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &cookie_ctx);
    mbedtls_ssl_conf_psk_cb(&conf, psk_callback, const_cast<void*>(static_cast<const void*>(&psk_lookup)));
    const uint32_t poll_ms = 250;  // how often stop / drop_session / idle are looked at
    g_fd = bio.fd;
    mbedtls_ssl_conf_read_timeout(&conf, poll_ms);

    AMBI_LOG("[dtls] listening on UDP %u (idle timeout %.1fs, mbedTLS %s)\n", static_cast<unsigned>(opts.port),
             opts.idle_timeout_s, MBEDTLS_VERSION_STRING);

    std::string old_ip;
    uint64_t old_packets = 0;
    bool old_announced = false;
    auto old_last = Clock::now(), old_notify = old_last;

    // One datagram of a DTLS session we do not have (the client kept streaming while we restarted).
    auto note_old = [&](const std::string& peer) {
        const auto now = Clock::now();
        if (old_ip != peer || std::chrono::duration<double>(now - old_last).count() > 10.0) {
            old_ip = peer;
            old_packets = 0;
            old_announced = false;
        }
        ++old_packets;
        old_last = now;
        if (!old_announced && old_packets >= 3) {
            old_announced = true;
            old_notify = now;
            AMBI_LOG("[dtls] %s is streaming from an OLD session (we were restarted while it streamed)\n", peer.c_str());
            if (opts.on_old_session) opts.on_old_session(old_ip, old_packets, true);
        } else if (old_announced && std::chrono::duration<double>(now - old_notify).count() >= 2.0) {
            old_notify = now;
            if (opts.on_old_session) opts.on_old_session(old_ip, old_packets, true);
        }
    };

    while (!stopping(opts)) {
        // ---- "accept": wait for a datagram and take its sender as the client
        bio.new_client_waiting = false;
        g_accept_turns++;
        {
            const int r = wait_readable(bio.fd, poll_ms);
            if (r == 0) continue;
            if (r < 0) {
                // v0.37: never retry at once - this task runs above WLED's loop
                if (g_select_errors++ % 100 == 0) AMBI_LOG("[dtls] select() failed (errno %d)\n", errno);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            socklen_t sl = sizeof(bio.peer);
            unsigned char head[5] = {0, 0, 0, 0, 0};  // DTLS record: type, version (2), epoch (2)
            const ssize_t got = recvfrom(bio.fd, head, sizeof(head), MSG_PEEK, reinterpret_cast<sockaddr*>(&bio.peer), &sl);
            if (got < 0) {
                if (g_peek_errors++ % 100 == 0) AMBI_LOG("[dtls] recvfrom(PEEK) failed (errno %d)\n", errno);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            // v0.37: only a handshake record of epoch 0 (a ClientHello) can start a session. Anything
            // else is a record of a session we do not have - the TV streaming on after we restarted.
            // Decided here from the header: the ESP's mbedTLS 2.28 reports such a record as "ClientHello
            // failed", not "unexpected message" (seen 2026-11-18), so the check after the handshake
            // never saw it - and a handshake was set up for every one of the TV's 25 packets/s.
            if (!(got >= 5 && head[0] == 22 && head[3] == 0 && head[4] == 0)) {
                unsigned char sink[8];
                recvfrom(bio.fd, sink, sizeof(sink), 0, nullptr, nullptr);
                note_old(ip_text(bio.peer));
                continue;
            }
        }
        const std::string peer = ip_text(bio.peer);

        mbedtls_ssl_context ssl;
        mbedtls_ssl_init(&ssl);
        Timer timer;
        ret = mbedtls_ssl_setup(&ssl, &conf);
        if (ret == 0) {
            mbedtls_ssl_set_timer_cb(&ssl, &timer, timer_set, timer_get);
            ret = mbedtls_ssl_set_client_transport_id(
                &ssl, reinterpret_cast<const unsigned char*>(&bio.peer.sin_addr.s_addr), 4);
        }
        if (ret != 0) {
            AMBI_LOG("[dtls] ssl setup failed: %s\n", err_text(ret).c_str());
            mbedtls_ssl_free(&ssl);
            unsigned char sink[8];  // do not spin on the same datagram
            recvfrom(bio.fd, sink, sizeof(sink), 0, nullptr, nullptr);
            continue;
        }
        mbedtls_ssl_set_bio(&ssl, &bio, bio_send, nullptr, bio_recv_timeout);

        // ---- handshake ------------------------------------------------------
        const auto hs_start = Clock::now();
        bool handshake_ok = false, cookie_round = false;
        while (!stopping(opts) && seconds_since(hs_start) < opts.handshake_timeout_s) {
            ret = mbedtls_ssl_handshake(&ssl);
            if (ret == 0) {
                handshake_ok = true;
                break;
            }
            if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
                cookie_round = true;  // the client's retry with the cookie is a new datagram: accept again
                break;
            }
            if (bio.new_client_waiting) break;
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_TIMEOUT) break;
        }
        if (cookie_round) {
            mbedtls_ssl_free(&ssl);
            continue;
        }
        if (!handshake_ok) {
            if (ret == MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE) {
                note_old(peer);  // not a ClientHello after all: a record of a session we do not have
            } else {
                const std::string why = bio.new_client_waiting ? "the client started over" :
                                        (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_TIMEOUT) ?
                                        "timed out" : err_text(ret);
                AMBI_LOG("[dtls] handshake from %s did not complete: %s\n", peer.c_str(), why.c_str());
                if (!bio.new_client_waiting && opts.on_handshake) opts.on_handshake(false, peer, why);
            }
            mbedtls_ssl_free(&ssl);
            continue;
        }

        if (old_packets > 0) {
            if (old_announced && opts.on_old_session) opts.on_old_session(old_ip, old_packets, false);
            old_ip.clear();
            old_packets = 0;
            old_announced = false;
        }
        AMBI_LOG("[dtls] handshake with %s complete in %.2f s, streaming\n", peer.c_str(), seconds_since(hs_start));
        if (opts.drop_session) opts.drop_session->store(false);  // an older request is not for this session
        if (opts.on_session) opts.on_session(true);
        if (opts.on_handshake) opts.on_handshake(true, peer, "");

        // ---- stream -----------------------------------------------------------
        std::vector<uint8_t> buf(1500);
        auto last_frame = Clock::now();
        for (;;) {
            if (stopping(opts)) break;
            if (opts.drop_session && opts.drop_session->exchange(false)) {
                AMBI_LOG("[dtls] stream.active changed: ending this session now\n");
                break;
            }
            if (bio.new_client_waiting) {
                AMBI_LOG("[dtls] %s started a new handshake: ending the open session\n", peer.c_str());
                break;
            }
            const int n = mbedtls_ssl_read(&ssl, buf.data(), buf.size());
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT) {
                if (seconds_since(last_frame) >= opts.idle_timeout_s) {
                    AMBI_LOG("[dtls] no data for %.1fs, ending session\n", seconds_since(last_frame));
                    break;
                }
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                AMBI_LOG("[dtls] client disconnected cleanly\n");
                break;
            }
            if (n < 0) {
                AMBI_LOG("[dtls] read error: %s\n", err_text(n).c_str());
                break;
            }
            if (n == 0) continue;
            last_frame = Clock::now();
            on_frame(buf.data(), static_cast<size_t>(n));
        }
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        AMBI_LOG("[dtls] session ended\n");
        if (opts.on_session) opts.on_session(false);
    }
    cleanup();
}

Stats stats() {
    Stats st;
    st.accept_turns = g_accept_turns;
    st.select_errors = g_select_errors;
    st.peek_errors = g_peek_errors;
    st.fd = g_fd;
    return st;
}

}  // namespace ambi_dtls
