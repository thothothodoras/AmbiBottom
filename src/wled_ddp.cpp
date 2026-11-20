#include "wled_ddp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace wled_ddp {

std::vector<std::vector<uint8_t>> build_packets(const std::vector<color_pipeline::RGB8>& leds, uint8_t seq) {
    std::vector<std::vector<uint8_t>> out;
    if (leds.empty()) return out;
    seq &= 0x0F;
    for (size_t start = 0; start < leds.size(); start += kMaxLedsPerPacket) {
        size_t n = std::min(kMaxLedsPerPacket, leds.size() - start);
        bool last = start + n == leds.size();
        uint32_t offset = static_cast<uint32_t>(start * 3);
        uint16_t len = static_cast<uint16_t>(n * 3);
        std::vector<uint8_t> p;
        p.reserve(kHeaderLen + len);
        p.push_back(static_cast<uint8_t>(0x40 | (last ? 0x01 : 0x00)));
        p.push_back(seq);
        p.push_back(0x0B);
        p.push_back(0x01);
        p.push_back(static_cast<uint8_t>(offset >> 24));
        p.push_back(static_cast<uint8_t>(offset >> 16));
        p.push_back(static_cast<uint8_t>(offset >> 8));
        p.push_back(static_cast<uint8_t>(offset));
        p.push_back(static_cast<uint8_t>(len >> 8));
        p.push_back(static_cast<uint8_t>(len));
        for (size_t i = start; i < start + n; ++i) {
            p.push_back(leds[i].r);
            p.push_back(leds[i].g);
            p.push_back(leds[i].b);
        }
        out.push_back(std::move(p));
    }
    return out;
}

Sender::~Sender() {
    if (fd_ >= 0) ::close(fd_);
}

bool Sender::open(const std::string& ip, uint16_t port) {
    static_assert(sizeof(addr_) >= sizeof(sockaddr_in), "addr_ too small");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        std::fprintf(stderr, "[wled] --wled-ip \"%s\" is not an IPv4 address; DDP output disabled\n", ip.c_str());
        return false;
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        std::fprintf(stderr, "[wled] socket() failed: %s; DDP output disabled\n", std::strerror(errno));
        return false;
    }
    std::memcpy(addr_, &a, sizeof(a));
    std::fprintf(stderr, "[wled] DDP output to %s:%u\n", ip.c_str(), port);
    return true;
}

bool Sender::send(const std::vector<color_pipeline::RGB8>& leds) {
    if (fd_ < 0 || leds.empty()) return false;
    bool ok = true;
    for (auto& p : build_packets(leds, seq_)) {
        ssize_t n = ::sendto(fd_, p.data(), p.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(addr_),
                             sizeof(sockaddr_in));
        if (n != static_cast<ssize_t>(p.size())) {
            ok = false;
            ++send_errors_;
            last_error_ = n < 0 ? std::strerror(errno) : "short write";
            // Log once, then again only after a success (no log flood if
            // WLED is unplugged and the network says unreachable at 25 fps).
            if (!error_logged_) {
                std::fprintf(stderr, "[wled] DDP send failed: %s\n", last_error_.c_str());
                error_logged_ = true;
            }
            break;
        }
    }
    if (ok) {
        last_error_.clear();
        if (error_logged_) std::fprintf(stderr, "[wled] DDP send working again\n");
        error_logged_ = false;
        ++frames_sent_;
    }
    seq_ = next_seq(seq_);
    return ok;
}

}  // namespace wled_ddp
