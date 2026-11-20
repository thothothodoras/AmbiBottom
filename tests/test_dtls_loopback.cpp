// End-to-end self-test: runs our own dtls_psk::run_server() on one thread
// and a minimal mbedTLS DTLS-PSK *client* on the main thread, both over
// 127.0.0.1. This is the thing that actually exercises mbedtls_net_accept(),
// the HelloVerifyRequest/cookie round trip, and the PSK callback for real,
// instead of just reasoning about the API from the headers. If this binary
// prints "LOOPBACK OK" and exits 0, the server side can be trusted to at
// least speak real DTLS-PSK correctly - a thing no amount of code review
// alone could establish.
//
// Two phases:
//   1. A client with a PAIRED identity handshakes, sends one real
//      HueStream frame, and we confirm the server's FrameHandler received
//      and correctly parsed exactly those bytes.
//   2. A client with an UNKNOWN identity must fail the handshake - this is
//      the security property the whole pairing model rests on ("the ESP
//      does its own fresh pairing"; an unpaired client must be rejected),
//      so it gets its own real (not just reasoned-about) check.
//
// NOTE: this does not and cannot prove the server will handshake
// successfully against the real TV's Hue Entertainment client specifically -
// only that it is a conformant DTLS-PSK server against a conformant client.
// The user's own TV is the only way to test that last mile.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/timing.h>
#include <mbedtls/error.h>

// Built twice: against the Linux bridge's server (ctest dtls_loopback) and,
// with AMBI_TEST_ESP_DTLS, against the ESP32 firmware's server file
// (firmware/wled-usermod/lib/AmbiBottomEsp/src/ambi_dtls.cpp, ctest esp_dtls).
#ifdef AMBI_TEST_ESP_DTLS
#include "ambi_dtls.h"
namespace dtls_psk = ambi_dtls;
#else
#include "dtls_psk_server.hpp"
#endif
#include "huestream.hpp"

namespace {

const std::string kTestIdentity = "self-test-user";
const std::vector<uint8_t> kTestKey = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
const std::string kUnknownIdentity = "someone-who-never-paired";
const std::vector<uint8_t> kUnknownKey = {
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
#ifdef AMBI_TEST_ESP_DTLS
const uint16_t kTestPort = 21002;
#else
const uint16_t kTestPort = 21000;
#endif

std::mutex g_mu;
std::condition_variable g_cv;
bool g_got_frame = false;
std::vector<uint8_t> g_frame_bytes;

void on_frame(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_frame_bytes.assign(data, data + len);
    g_got_frame = true;
    g_cv.notify_all();
}

std::vector<uint8_t> build_test_frame() {
    // One HueStream v1 XY+bri frame, lamp "1", a mid-brightness warm colour.
    std::vector<uint8_t> f;
    const char* magic = "HueStream";
    f.insert(f.end(), magic, magic + 9);
    f.push_back(1);  // major
    f.push_back(0);  // minor
    f.push_back(42); // seq
    f.push_back(0); f.push_back(0);  // reserved
    f.push_back(1);  // colorspace = XY+bri
    f.push_back(0);  // reserved, pad to 16
    f.push_back(0);               // type
    f.push_back(0); f.push_back(1);  // light_id = 1
    uint16_t x16 = 30000, y16 = 25000, bri16 = 50000;
    f.push_back(x16 >> 8); f.push_back(x16 & 0xFF);
    f.push_back(y16 >> 8); f.push_back(y16 & 0xFF);
    f.push_back(bri16 >> 8); f.push_back(bri16 & 0xFF);
    return f;
}

std::string err_str(int ret) {
    char buf[256];
    mbedtls_strerror(ret, buf, sizeof(buf));
    return std::string(buf);
}

// Runs one client handshake attempt against the server. Returns true iff
// the handshake itself succeeded (ret == 0 from mbedtls_ssl_handshake).
// On success, if send_frame is true, also writes one HueStream frame.
bool run_client(const std::string& identity, const std::vector<uint8_t>& key, bool send_frame) {
    mbedtls_net_context server_fd;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_timing_delay_context timer;

    mbedtls_net_init(&server_fd);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    auto cleanup = [&] {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_net_free(&server_fd);
    };

    const std::string pers = "ambi-dtls-psk-client-" + identity;
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     reinterpret_cast<const unsigned char*>(pers.data()), pers.size());
    if (ret != 0) { std::fprintf(stderr, "[client:%s] ctr_drbg_seed: %s\n", identity.c_str(), err_str(ret).c_str()); cleanup(); return false; }

    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", kTestPort);
    ret = mbedtls_net_connect(&server_fd, "127.0.0.1", port_str, MBEDTLS_NET_PROTO_UDP);
    if (ret != 0) { std::fprintf(stderr, "[client:%s] net_connect: %s\n", identity.c_str(), err_str(ret).c_str()); cleanup(); return false; }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { cleanup(); return false; }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);  // PSK, no certs involved
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&conf, 2000);

    ret = mbedtls_ssl_conf_psk(&conf, key.data(), key.size(),
                                reinterpret_cast<const unsigned char*>(identity.data()), identity.size());
    if (ret != 0) { cleanup(); return false; }

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) { cleanup(); return false; }

    mbedtls_ssl_set_timer_cb(&ssl, &timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

    std::fprintf(stderr, "[client:%s] handshaking...\n", identity.c_str());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        ret = mbedtls_ssl_handshake(&ssl);
    } while ((ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) &&
             std::chrono::steady_clock::now() < deadline);

    if (ret != 0) {
        std::fprintf(stderr, "[client:%s] handshake failed (expected for an unpaired identity): %s\n",
                     identity.c_str(), err_str(ret).c_str());
        cleanup();
        return false;
    }
    std::fprintf(stderr, "[client:%s] handshake OK, ciphersuite=%s\n", identity.c_str(),
                 mbedtls_ssl_get_ciphersuite(&ssl));

    if (send_frame) {
        std::vector<uint8_t> frame = build_test_frame();
        int n;
        do {
            n = mbedtls_ssl_write(&ssl, frame.data(), frame.size());
        } while (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE);
        if (n < 0) {
            std::fprintf(stderr, "[client:%s] ssl_write failed: %s\n", identity.c_str(), err_str(n).c_str());
            cleanup();
            return false;
        }
        std::fprintf(stderr, "[client:%s] sent %d-byte frame\n", identity.c_str(), n);

        {
            std::unique_lock<std::mutex> lk(g_mu);
            bool ok = g_cv.wait_for(lk, std::chrono::seconds(5), [] { return g_got_frame; });
            if (!ok) {
                std::fprintf(stderr, "FAIL: server never delivered the frame to on_frame()\n");
                cleanup();
                return false;
            }
        }

        if (g_frame_bytes != frame) {
            std::fprintf(stderr, "FAIL: frame bytes received by server don't match what client sent\n");
            cleanup();
            return false;
        }
        huestream::Frame parsed;
        if (!huestream::parse_frame(g_frame_bytes, parsed) || parsed.channels.size() != 1 ||
            parsed.channels[0].lamp_id != "1" || parsed.colorspace != 1) {
            std::fprintf(stderr, "FAIL: parsed frame content is wrong\n");
            cleanup();
            return false;
        }
    }

    mbedtls_ssl_close_notify(&ssl);
    cleanup();
    return true;
}

}  // namespace

int main() {
    std::thread server_thread([] {
        dtls_psk::Options opts;
        opts.port = kTestPort;
        opts.idle_timeout_s = 5.0;
        opts.handshake_timeout_s = 10.0;
        dtls_psk::run_server(
            opts,
            [](const std::string& id) -> std::vector<uint8_t> {
                if (id == kTestIdentity) return kTestKey;
                return {};  // unknown identity -> handshake fails
            },
            on_frame);
    });
    server_thread.detach();  // run_server() loops forever; we just exit the process when done

    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let it bind

    // --- phase 1: paired client handshakes and streams a real frame ----
    if (!run_client(kTestIdentity, kTestKey, /*send_frame=*/true)) {
        std::fprintf(stderr, "FAIL: paired client could not complete the handshake/frame exchange\n");
        return 1;
    }

    // --- phase 2: unpaired client must be rejected ----------------------
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_got_frame = false;
        g_frame_bytes.clear();
    }
    bool unpaired_accepted = run_client(kUnknownIdentity, kUnknownKey, /*send_frame=*/false);
    if (unpaired_accepted) {
        std::fprintf(stderr, "FAIL: server accepted a handshake from an unpaired/unknown PSK identity "
                              "- this would mean any device on the LAN could stream to the strip\n");
        return 1;
    }
    std::fprintf(stderr, "[ok] unpaired identity was correctly rejected\n");

    std::printf("LOOPBACK OK: real DTLS-PSK handshake + HueStream frame delivered end-to-end "
                "over 127.0.0.1:%u, and an unpaired identity was correctly rejected\n", kTestPort);
    std::exit(0);  // server thread loops forever by design; just end the process
}
