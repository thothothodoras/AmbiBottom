// HueStream frame parser (v1 and v2).
//
// Byte layout cross-confirmed from two independent, already-tested sources:
//   - 83noit/ha-hue-entertainment (Python/OpenSSL, MIT) — const.py / entertainment.py
//   - the user's own fakebridge.py v1.6, parse_frame() — already validated against
//     the real Philips 65OLED707 TV via `openssl s_server -dtls`.
//
// v1: "HueStream" (9) + major(1) + minor(1) + seq(1) + 2 reserved + colorspace(1)
//     + 2 reserved = 16-byte header, then 9 bytes/channel:
//       type(1, must be 0) + light_id(u16 BE) + R/X(u16 BE) + G/Y(u16 BE) + B/bri(u16 BE)
// v2: 16-byte HueStream header + 36-byte area/entertainment-configuration id
//     = 52 bytes total, then 7 bytes/channel:
//       channel_id(1) + val1(u16 BE) + val2(u16 BE) + val3(u16 BE)
//
// colorspace: 0 = RGB (values are 16-bit, use high byte), 1 = XY + brightness
// (x = val1/65535, y = val2/65535, bri = val3/65535).
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace huestream {

struct Channel {
    std::string lamp_id;  // v1: light_id as decimal string; v2: channel index + 1
    uint16_t a, b, c;     // raw 16-bit values: RGB (R,G,B) or XY+bri (x16,y16,bri16)
};

struct Frame {
    uint8_t seq = 0;
    uint8_t major = 0, minor = 0;
    uint8_t colorspace = 0;  // 0 = RGB, 1 = XY+bri
    std::vector<Channel> channels;
};

// Returns false if the frame is malformed (bad magic, too short, unknown
// version, duplicate lamp ids, unexpected type byte in v1). On success,
// `out` is filled and the function returns true. Pure function, no I/O.
bool parse_frame(const uint8_t* data, size_t len, Frame& out);

// Convenience overload.
inline bool parse_frame(const std::vector<uint8_t>& data, Frame& out) {
    return parse_frame(data.data(), data.size(), out);
}

}  // namespace huestream
