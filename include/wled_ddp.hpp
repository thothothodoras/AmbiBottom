// DDP output to a WLED controller (Phase 1 of docs/PLAN-NEXT.md).
//
// The LXC bridge keeps doing everything (pairing, DTLS, colour pipeline)
// and, when --wled-ip is given, sends every frame's render_strip() result
// to WLED's always-on DDP listener (UDP 4048: WLED
// wled00/src/dependencies/e131/ESPAsyncE131.h:51, see docs/WLED-ANALYSIS.md (e)).
//
// DDP header (10 bytes, http://www.3waylabs.com/ddp/):
//   [0] flags  0x40 = version 1, 0x01 = PUSH (display now)
//   [1] seq    low 4 bits, 1..15 (0 = unused)
//   [2] type   0x0B = RGB, 8 bit per channel
//   [3] id     0x01 = default output device
//   [4..7]     data offset in BYTES, big-endian
//   [8..9]     data length in bytes, big-endian
// WLED accepts at most 480 LEDs (1440 bytes) per packet
// (ESPAsyncE131.h:63); longer strips are split, PUSH only on the last one.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "color_pipeline.hpp"

namespace wled_ddp {

constexpr uint16_t kDefaultPort = 4048;
constexpr size_t kHeaderLen = 10;
constexpr size_t kMaxLedsPerPacket = 480;

// Pure: builds the packets for one frame. `seq` is the 1..15 sequence
// number for this frame (same in every packet of the frame).
std::vector<std::vector<uint8_t>> build_packets(const std::vector<color_pipeline::RGB8>& leds, uint8_t seq);

// Next DDP sequence number after `seq` (1..15, wraps 15 -> 1, never 0).
inline uint8_t next_seq(uint8_t seq) { return static_cast<uint8_t>(seq >= 15 ? 1 : seq + 1); }

// UDP sender. Not thread-safe; call from one thread (the frame handler,
// which already runs under frame_mu).
class Sender {
public:
    Sender() = default;
    ~Sender();
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    // Returns false (and logs) if the IP is not a valid IPv4 address or
    // the socket cannot be created.
    bool open(const std::string& ip, uint16_t port);
    bool is_open() const { return fd_ >= 0; }

    // Sends one frame. Never blocks on a full socket buffer (MSG_DONTWAIT):
    // a dropped frame is better than stalling the DTLS thread.
    bool send(const std::vector<color_pipeline::RGB8>& leds);

    uint64_t frames_sent() const { return frames_sent_; }
    uint64_t send_errors() const { return send_errors_; }
    const std::string& last_error() const { return last_error_; }  // empty after a good send

private:
    int fd_ = -1;
    uint8_t seq_ = 1;
    uint64_t frames_sent_ = 0;
    uint64_t send_errors_ = 0;
    bool error_logged_ = false;
    std::string last_error_;
    unsigned char addr_[16] = {0};  // sockaddr_in storage (kept opaque in the header)
};

}  // namespace wled_ddp
