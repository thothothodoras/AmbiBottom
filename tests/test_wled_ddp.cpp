// Tests for the Phase 1 DDP output (include/wled_ddp.hpp): packet layout
// as WLED's parser reads it (wled00/e131.cpp handleDDPPacket) and a real
// UDP loopback send.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <vector>

#include "wled_ddp.hpp"

using color_pipeline::RGB8;

static std::vector<RGB8> ramp(size_t n) {
    std::vector<RGB8> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = {uint8_t(i), uint8_t(i * 3), uint8_t(255 - i)};
    return v;
}

static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

static void test_43_leds_one_packet() {
    auto leds = ramp(43);
    auto pk = wled_ddp::build_packets(leds, 7);
    assert(pk.size() == 1);
    auto& p = pk[0];
    assert(p.size() == 10 + 43 * 3);
    assert(p[0] == 0x41);           // version 1 + PUSH
    assert(p[1] == 7);              // seq
    assert(p[2] == 0x0B);           // RGB24
    assert(p[3] == 0x01);           // default output
    assert(be32(&p[4]) == 0);       // byte offset
    assert(be16(&p[8]) == 129);     // data length in bytes
    for (size_t i = 0; i < 43; ++i) {
        assert(p[10 + 3 * i] == leds[i].r);
        assert(p[10 + 3 * i + 1] == leds[i].g);
        assert(p[10 + 3 * i + 2] == leds[i].b);
    }
    std::printf("[ok] 43 LEDs -> one 139-byte DDP packet, PUSH set, colours in order\n");
}

static void test_long_strip_split() {
    auto leds = ramp(1000);
    auto pk = wled_ddp::build_packets(leds, 3);
    assert(pk.size() == 3);  // 480 + 480 + 40
    const uint32_t offs[] = {0, 1440, 2880};
    const uint16_t lens[] = {1440, 1440, 120};
    for (int i = 0; i < 3; ++i) {
        assert(be32(&pk[i][4]) == offs[i]);
        assert(be16(&pk[i][8]) == lens[i]);
        assert(pk[i].size() == 10u + lens[i]);
        assert(pk[i][1] == 3);
        assert((pk[i][0] & 0x01) == (i == 2 ? 1 : 0));  // PUSH only on the last
        assert((pk[i][0] & 0xC0) == 0x40);
    }
    assert(pk[1][10] == leds[480].r && pk[2][10 + 3 * 39 + 2] == leds[999].b);
    std::printf("[ok] 1000 LEDs -> 480/480/40, byte offsets 0/1440/2880, PUSH only on last\n");
}

static void test_empty_and_seq() {
    assert(wled_ddp::build_packets({}, 1).empty());
    assert(wled_ddp::next_seq(1) == 2);
    assert(wled_ddp::next_seq(14) == 15);
    assert(wled_ddp::next_seq(15) == 1);  // never 0 (0 = "unused" in DDP)
    std::printf("[ok] empty strip sends nothing; seq wraps 15 -> 1\n");
}

static void test_sender_loopback() {
    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    assert(rx >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    assert(bind(rx, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    socklen_t al = sizeof(a);
    getsockname(rx, reinterpret_cast<sockaddr*>(&a), &al);
    timeval tv{1, 0};
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    wled_ddp::Sender s;
    assert(!s.is_open());
    assert(!s.send(ramp(43)));  // not open: no-op
    assert(s.open("127.0.0.1", ntohs(a.sin_port)));
    auto leds = ramp(43);
    uint8_t buf[2048];
    for (int f = 0; f < 17; ++f) {
        assert(s.send(leds));
        ssize_t n = recv(rx, buf, sizeof(buf), 0);
        assert(n == 139);
        assert(buf[0] == 0x41);
        assert(buf[1] == uint8_t(f % 15 + 1));  // 1..15, 1..
        assert(buf[10] == leds[0].r && buf[138] == leds[42].b);
    }
    assert(s.frames_sent() == 17 && s.send_errors() == 0);
    close(rx);

    wled_ddp::Sender bad;
    assert(!bad.open("not-an-ip", 4048));
    assert(!bad.is_open());
    std::printf("[ok] Sender: 17 frames over real UDP loopback, seq 1..15 wrap; bad IP rejected\n");
}

int main() {
    test_43_leds_one_packet();
    test_long_strip_split();
    test_empty_and_seq();
    test_sender_loopback();
    std::printf("all wled_ddp tests passed\n");
    return 0;
}
