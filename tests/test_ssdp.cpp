// End-to-end self-test for the SSDP responder: runs mdns_ssdp::run_ssdp_responder()
// on its own thread, sends a real M-SEARCH multicast datagram from a
// separate socket, and checks that all 3 expected ST variants come back -
// same "actually run it" pattern as the DTLS and Hue REST self-tests,
// which is what caught real bugs in both of those.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>

#include "mdns_ssdp.hpp"

namespace {
[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}
}  // namespace

int main() {
    auto identity = bridge_identity::make_identity("127.0.0.1", 80);

    std::thread ssdp_thread([&] { mdns_ssdp::run_ssdp_responder(identity); });
    ssdp_thread.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    int client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (client < 0) fail("client socket() failed");
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;  // let the kernel pick, so we can recv the replies on it
    if (bind(client, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) fail("client bind() failed");

    // The responder joined the multicast group on the "127.0.0.1" interface
    // (identity.ip below); without telling this client socket to send out
    // that same interface, the kernel would pick the default route (not
    // lo) and the responder would never see the datagram.
    in_addr via_lo{};
    inet_pton(AF_INET, "127.0.0.1", &via_lo);
    setsockopt(client, IPPROTO_IP, IP_MULTICAST_IF, &via_lo, sizeof(via_lo));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);

    std::string msearch =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\nST: ssdp:all\r\n\r\n";
    if (sendto(client, msearch.data(), msearch.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
        fail("sendto M-SEARCH failed");
    }

    timeval tv{2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::set<std::string> seen_st;
    for (int i = 0; i < 5; ++i) {
        char buf[2048];
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        std::fprintf(stderr, "[test] recv #%d -> n=%zd\n", i, n);
        if (n <= 0) break;
        buf[n] = '\0';
        std::string msg(buf, static_cast<size_t>(n));
        // Anchor on "\r\nST: " (a line start), not bare "ST: " - the latter
        // also matches inside "HOST: 239.255.255.250:1900" on the first
        // line of every message, which silently extracted the same bogus
        // "239.255.255.250:1900" value 3 times (caught by this test's own
        // first run: it reported only 1 distinct value even though the
        // raw dumps showed 3 genuinely different ST lines).
        size_t st_pos = msg.find("\r\nST: ");
        if (st_pos != std::string::npos) {
            st_pos += 2;  // skip the \r\n
            size_t end = msg.find("\r\n", st_pos);
            seen_st.insert(msg.substr(st_pos + 4, end - st_pos - 4));
        }
        if (msg.find(identity.bridgeid) == std::string::npos) fail("response missing hue-bridgeid");
        if (msg.find("/description.xml") == std::string::npos) fail("response missing LOCATION description.xml");
    }
    close(client);

    if (seen_st.size() != 3) {
        std::fprintf(stderr, "saw %zu distinct ST values, expected 3\n", seen_st.size());
        fail("did not receive all 3 expected SSDP response variants");
    }
    if (!seen_st.count("upnp:rootdevice")) fail("missing ST: upnp:rootdevice variant");

    std::printf("SSDP OK: M-SEARCH answered with all 3 expected ST variants, each carrying "
                "the correct bridgeid and description.xml location\n");
    return 0;
}
