#include "dtls_psk_server.hpp"

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/timing.h>
#include <mbedtls/error.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dtls_psk {

namespace {

std::string mbedtls_err_str(int ret) {
    char buf[256];
    mbedtls_strerror(ret, buf, sizeof(buf));
    return std::string(buf);
}

// Bound to mbedtls_ssl_conf_psk_cb: mbedTLS asks us, per-handshake, to
// supply the key for the identity the client presented. We look it up and
// hand it to mbedTLS via mbedtls_ssl_set_hs_psk(); returning nonzero fails
// the handshake exactly like a real Hue bridge rejecting an unpaired
// client (no separate error path needed for "unknown identity").
int psk_callback(void* p_psk, mbedtls_ssl_context* ssl,
                  const unsigned char* identity, size_t identity_len) {
    const auto* lookup = static_cast<const PskLookup*>(p_psk);
    std::string id(reinterpret_cast<const char*>(identity), identity_len);
    std::vector<uint8_t> key = (*lookup)(id);
    if (key.empty()) {
        return -1;
    }
    return mbedtls_ssl_set_hs_psk(ssl, key.data(), key.size());
}

// ---- v0.29: saved session (file layout) ----------------------------------
// "AMBIDTLS1\n" magic, then: 4 bytes peer IPv4, 2 bytes peer port (network
// order), 8 bytes save time (unix seconds, little endian), then the
// mbedtls_ssl_context_save() blob.
const char kMagic[] = "AMBIDTLS1\n";

struct Saved {
    bool ok = false;
    unsigned char ip[4] = {0};
    unsigned char port[2] = {0};
    int64_t saved_at = 0;
    std::vector<unsigned char> blob;
};

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool write_saved(const std::string& path, const unsigned char ip[4], const unsigned char port[2],
                 const unsigned char* blob, size_t len) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    fchmod(fd, 0600);
    std::string out(kMagic, sizeof(kMagic) - 1);
    out.append(reinterpret_cast<const char*>(ip), 4);
    out.append(reinterpret_cast<const char*>(port), 2);
    int64_t t = unix_now();
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((t >> (8 * i)) & 0xFF));
    out.append(reinterpret_cast<const char*>(blob), len);
    bool ok = ::write(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size());
    ::fsync(fd);
    ::close(fd);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// Reads AND deletes the file (a session is used at most once).
Saved take_saved(const std::string& path) {
    Saved sv;
    std::ifstream f(path, std::ios::binary);
    if (!f) return sv;
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    ::unlink(path.c_str());
    std::string d = ss.str();
    const size_t head = sizeof(kMagic) - 1 + 4 + 2 + 8;
    if (d.size() <= head || d.compare(0, sizeof(kMagic) - 1, kMagic) != 0) return sv;
    size_t o = sizeof(kMagic) - 1;
    std::memcpy(sv.ip, d.data() + o, 4);
    std::memcpy(sv.port, d.data() + o + 4, 2);
    for (int i = 0; i < 8; ++i)
        sv.saved_at |= static_cast<int64_t>(static_cast<unsigned char>(d[o + 6 + i])) << (8 * i);
    sv.blob.assign(d.begin() + static_cast<std::ptrdiff_t>(head), d.end());
    sv.ok = true;
    return sv;
}

std::string ip_str(const unsigned char* ip, size_t len) {
    char b[48] = "?";
    if (len == 4) std::snprintf(b, sizeof(b), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return b;
}

}  // namespace

void run_server(const Options& opts, const PskLookup& psk_lookup, const FrameHandler& on_frame) {
    mbedtls_net_context listen_fd;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_cookie_ctx cookie_ctx;

    mbedtls_net_init(&listen_fd);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_cookie_init(&cookie_ctx);

    const char* pers = "ambi-dtls-psk-server";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     reinterpret_cast<const unsigned char*>(pers), std::strlen(pers));
    if (ret != 0) {
        std::fprintf(stderr, "[dtls] ctr_drbg_seed failed: %s\n", mbedtls_err_str(ret).c_str());
        return;
    }

    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", opts.port);
    ret = mbedtls_net_bind(&listen_fd, nullptr, port_str, MBEDTLS_NET_PROTO_UDP);
    if (ret != 0) {
        std::fprintf(stderr, "[dtls] bind udp/%u failed: %s\n", opts.port, mbedtls_err_str(ret).c_str());
        return;
    }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        std::fprintf(stderr, "[dtls] ssl_config_defaults failed: %s\n", mbedtls_err_str(ret).c_str());
        return;
    }
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    ret = mbedtls_ssl_cookie_setup(&cookie_ctx, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        std::fprintf(stderr, "[dtls] cookie_setup failed: %s\n", mbedtls_err_str(ret).c_str());
        return;
    }
    mbedtls_ssl_conf_dtls_cookies(&conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &cookie_ctx);

    mbedtls_ssl_conf_psk_cb(&conf, psk_callback, const_cast<void*>(static_cast<const void*>(&psk_lookup)));

    // Per-recv block time (how long one mbedtls_net_recv_timeout() call
    // waits before returning WANT_READ). Kept short so our own idle/
    // handshake deadlines below are checked often, not so the protocol
    // itself only allows 2s round-trips.
    // v0.31: 2000 -> 250 ms, so stop/drop_session are seen within 0.25 s.
    const uint32_t poll_ms = 250;
    mbedtls_ssl_conf_read_timeout(&conf, poll_ms);

    std::fprintf(stderr, "[dtls] listening on UDP %u (idle timeout %.1fs)\n",
                 opts.port, opts.idle_timeout_s);

    // v0.29: a session saved by the previous run (stop/start/restart/exit)
    Saved resume;
    if (!opts.session_file.empty()) {
        resume = take_saved(opts.session_file);
        if (resume.ok) {
            const double age = static_cast<double>(unix_now() - resume.saved_at);
            if (age < 0 || age > opts.resume_max_age_s) {
                std::fprintf(stderr, "[dtls] saved session too old (%.0f s) - not used\n", age);
                resume = Saved();
            } else {
                std::fprintf(stderr, "[dtls] saved session of %s:%u found (%.0f s old): it continues when that "
                                     "TV sends again\n", ip_str(resume.ip, 4).c_str(),
                             (resume.port[0] << 8) | resume.port[1], age);
            }
        }
    }

    // v0.21: old-session detection (see OldSessionHandler). While a client
    // keeps sending records from an old session, every datagram is a
    // failed "handshake" - logged once with what to do, then summarised.
    std::string old_ip;
    uint64_t old_packets = 0;
    bool old_announced = false;
    auto old_last = std::chrono::steady_clock::now();
    auto old_report = old_last, old_notify = old_last;

    for (;;) {
        mbedtls_net_context client_fd;
        mbedtls_net_init(&client_fd);
        mbedtls_ssl_context ssl;
        mbedtls_ssl_init(&ssl);
        mbedtls_timing_delay_context timer;

        ret = mbedtls_ssl_setup(&ssl, &conf);
        if (ret != 0) {
            std::fprintf(stderr, "[dtls] ssl_setup failed: %s\n", mbedtls_err_str(ret).c_str());
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&client_fd);
            continue;
        }
        mbedtls_ssl_set_timer_cb(&ssl, &timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);

        unsigned char client_ip[16] = {0};
        size_t cliip_len = 0;
        if (!old_announced) std::fprintf(stderr, "[dtls] waiting for a client...\n");
        ret = mbedtls_net_accept(&listen_fd, &client_fd, client_ip, sizeof(client_ip), &cliip_len);
        if (ret != 0) {
            std::fprintf(stderr, "[dtls] net_accept failed: %s\n", mbedtls_err_str(ret).c_str());
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&client_fd);
            continue;
        }

        ret = mbedtls_ssl_set_client_transport_id(&ssl, client_ip, cliip_len);
        if (ret != 0) {
            std::fprintf(stderr, "[dtls] set_client_transport_id failed: %s\n", mbedtls_err_str(ret).c_str());
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&client_fd);
            continue;
        }

        mbedtls_ssl_set_bio(&ssl, &client_fd, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

        // v0.29: the first datagram from the TV whose session we saved: load
        // that session instead of a handshake (the datagram is only peeked by
        // net_accept, so ssl_read below decrypts it).
        bool resumed = false;
        unsigned char peer_port[2] = {0, 0};
        {
            sockaddr_storage pa{};
            socklen_t pl = sizeof(pa);
            if (getpeername(client_fd.fd, reinterpret_cast<sockaddr*>(&pa), &pl) == 0 && pa.ss_family == AF_INET) {
                const auto* in = reinterpret_cast<const sockaddr_in*>(&pa);
                std::memcpy(peer_port, &in->sin_port, 2);
            }
        }
        if (resume.ok && cliip_len == 4 && std::memcmp(client_ip, resume.ip, 4) == 0 &&
            std::memcmp(peer_port, resume.port, 2) == 0) {
            int lr = mbedtls_ssl_context_load(&ssl, resume.blob.data(), resume.blob.size());
            resume = Saved();  // one try only
            if (lr == 0) {
                resumed = true;
            } else {
                std::fprintf(stderr, "[dtls] saved session could not be loaded: %s\n", mbedtls_err_str(lr).c_str());
                mbedtls_ssl_free(&ssl);
                mbedtls_net_free(&client_fd);
                continue;  // the next datagram goes the normal way (old-session detection)
            }
        }

        // Handshake, with our own wall-clock deadline on top of mbedTLS's
        // internal per-packet retransmission timers.
        //
        // On MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED we do NOT retry
        // mbedtls_ssl_handshake() on this same ssl/client_fd pair: that was
        // tried first and the loopback self-test alongside this file caught
        // it failing with "Bad input parameters to function" on the retry
        // (session_reset() apparently invalidates more state than just the
        // handshake progress - plausibly the client_transport_id binding
        // the cookie check relies on). mbedTLS's own reference DTLS server
        // example (programs/ssl/dtls_server.c) doesn't retry in place
        // either: after HELLO_VERIFY_REQUIRED it resets and goes back to
        // accept() for a new client_fd, since the client's cookie-bearing
        // ClientHello retransmission shows up as a fresh datagram on the
        // listening socket. We do the same here, confirmed working by that
        // same self-test.
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration<double>(opts.handshake_timeout_s);
        bool handshake_ok = resumed;
        bool needs_reaccept = false;
        while (!resumed && std::chrono::steady_clock::now() < deadline) {
            ret = mbedtls_ssl_handshake(&ssl);
            if (ret == 0) {
                handshake_ok = true;
                break;
            }
            if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
                needs_reaccept = true;
                break;  // go back to mbedtls_net_accept(), not another handshake() here
            }
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                break;  // real failure, not a retry condition
            }
        }

        if (needs_reaccept) {
            std::fprintf(stderr, "[dtls] sent HelloVerifyRequest, waiting for client's cookie retry\n");
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&client_fd);
            continue;
        }

        char peer[48] = "?";
        if (cliip_len == 4) {
            std::snprintf(peer, sizeof(peer), "%u.%u.%u.%u", client_ip[0], client_ip[1], client_ip[2], client_ip[3]);
        }
        if (!handshake_ok && ret == MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE) {
            auto now = std::chrono::steady_clock::now();
            if (old_ip != peer || std::chrono::duration<double>(now - old_last).count() > 10.0) {
                old_ip = peer;  // a new streak
                old_packets = 0;
                old_announced = false;
            }
            ++old_packets;
            old_last = now;
            if (!old_announced && old_packets >= 3) {
                old_announced = true;
                old_report = old_notify = now;
                std::fprintf(stderr,
                             "[dtls] %s is streaming from an OLD session (this bridge was restarted while it "
                             "was streaming; it cannot be decrypted). On the TV turn Ambilight+hue off and on.\n",
                             peer);
                if (opts.on_old_session) opts.on_old_session(old_ip, old_packets, true);
            } else if (old_announced) {
                if (std::chrono::duration<double>(now - old_notify).count() >= 2.0) {
                    old_notify = now;
                    if (opts.on_old_session) opts.on_old_session(old_ip, old_packets, true);
                }
                if (std::chrono::duration<double>(now - old_report).count() >= 30.0) {
                    old_report = now;
                    std::fprintf(stderr, "[dtls] %s: still the old session (%llu packets ignored) - turn "
                                         "Ambilight+hue off and on on the TV\n",
                                 peer, static_cast<unsigned long long>(old_packets));
                }
            }
            if (!old_announced) {
                std::fprintf(stderr, "[dtls] handshake did not complete: %s\n", mbedtls_err_str(ret).c_str());
            }
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&client_fd);
            continue;
        }

        if (!handshake_ok) {
            std::fprintf(stderr, "[dtls] handshake did not complete: %s\n", mbedtls_err_str(ret).c_str());
            if (opts.on_handshake) opts.on_handshake(false, peer, mbedtls_err_str(ret));
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&client_fd);
            continue;
        }

        if (old_packets > 0) {
            if (old_announced) {
                std::fprintf(stderr, "[dtls] new session from %s after %llu old-session packets\n", peer,
                             static_cast<unsigned long long>(old_packets));
                if (opts.on_old_session) opts.on_old_session(old_ip, old_packets, false);
            }
            old_ip.clear();
            old_packets = 0;
            old_announced = false;
        }
        if (resumed)
            std::fprintf(stderr, "[dtls] %s: saved session continued (no new handshake), streaming\n", peer);
        else
            std::fprintf(stderr, "[dtls] handshake complete, streaming\n");
        if (opts.in_session) opts.in_session->store(true);
        // A drop request from before this session started is not for it.
        if (opts.drop_session) opts.drop_session->store(false);
        if (opts.on_handshake) opts.on_handshake(true, peer, resumed ? "saved session continued" : "");
        std::vector<uint8_t> buf(2048);
        auto last_frame = std::chrono::steady_clock::now();
        bool stopping = false;
        uint64_t frames = 0;
        for (;;) {
            if (opts.stop && opts.stop->load()) {
                stopping = true;
                break;
            }
            if (opts.drop_session && opts.drop_session->exchange(false)) {
                std::fprintf(stderr, "[dtls] stream.active changed: ending this session now so the next "
                                     "handshake is not kept waiting\n");
                break;
            }
            int n = mbedtls_ssl_read(&ssl, buf.data(), buf.size());
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                auto idle = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_frame).count();
                if (idle >= opts.idle_timeout_s) {
                    std::fprintf(stderr, "[dtls] no data for %.1fs, ending session%s\n", idle,
                                 resumed && frames == 0 ? " (the saved session did not match what the TV sends)" : "");
                    break;
                }
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                std::fprintf(stderr, "[dtls] client disconnected cleanly\n");
                break;
            }
            if (n < 0) {
                std::fprintf(stderr, "[dtls] ssl_read error: %s\n", mbedtls_err_str(n).c_str());
                break;
            }
            if (n == 0) {
                continue;  // zero-length datagram, nothing to do
            }
            last_frame = std::chrono::steady_clock::now();
            if (resumed && frames == 0) {
                std::fprintf(stderr, "[dtls] %s: first frame decrypted with the saved session\n", peer);
                if (opts.on_resumed) opts.on_resumed();
            }
            ++frames;
            on_frame(buf.data(), static_cast<size_t>(n));
        }

        // v0.29: on stop, save the session for the next run instead of ending it
        // (the TV ignored close_notify on the real setup, 2026-11-06).
        bool saved = false;
        if (stopping && !opts.session_file.empty()) {
            std::vector<unsigned char> blob(4096);
            size_t olen = 0;
            int sr = mbedtls_ssl_context_save(&ssl, blob.data(), blob.size(), &olen);
            if (sr == 0 && cliip_len == 4 && write_saved(opts.session_file, client_ip, peer_port, blob.data(), olen)) {
                saved = true;
                std::fprintf(stderr, "[dtls] shutting down: session with %s saved (%zu bytes) - the next start "
                                     "continues it, no close_notify\n", peer, olen);
            } else {
                std::fprintf(stderr, "[dtls] shutting down: session NOT saved (%s) - sending close_notify\n",
                             sr != 0 ? mbedtls_err_str(sr).c_str() : "file write failed");
            }
        }
        if (!saved) {
            int cn = mbedtls_ssl_close_notify(&ssl);
            if (stopping)
                std::fprintf(stderr, "[dtls] shutting down: close_notify %s to %s\n", cn == 0 ? "sent" : "FAILED", peer);
        }
        mbedtls_ssl_free(&ssl);
        mbedtls_net_free(&client_fd);
        std::fprintf(stderr, "[dtls] session ended\n");
        if (opts.in_session) opts.in_session->store(false);
        if (stopping) return;
    }
}

}  // namespace dtls_psk
