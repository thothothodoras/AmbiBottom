#include "huestream.hpp"
#include <cstring>

namespace huestream {

static uint16_t be16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

bool parse_frame(const uint8_t* data, size_t len, Frame& out) {
    static const char MARK[] = "HueStream";
    static const size_t MARK_LEN = 9;

    if (len < 16 || std::memcmp(data, MARK, MARK_LEN) != 0) {
        return false;
    }

    const uint8_t major = data[9];
    const uint8_t minor = data[10];
    const uint8_t seq = data[11];
    const uint8_t colorspace = data[14];
    if (colorspace != 0 && colorspace != 1) {
        return false;
    }

    out.seq = seq;
    out.major = major;
    out.minor = minor;
    out.colorspace = colorspace;
    out.channels.clear();

    if (major == 1 && len > 16 && (len - 16) % 9 == 0) {
        for (size_t o = 16; o < len; o += 9) {
            if (data[o] != 0) {
                return false;  // v1 type byte must be 0 (light, not reserved)
            }
            Channel ch;
            ch.lamp_id = std::to_string(be16(data + o + 1));
            ch.a = be16(data + o + 3);
            ch.b = be16(data + o + 5);
            ch.c = be16(data + o + 7);
            out.channels.push_back(ch);
        }
    } else if (major == 2 && len > 52 && (len - 52) % 7 == 0) {
        for (size_t o = 52; o < len; o += 7) {
            Channel ch;
            // channel byte i -> lamp (i+1), matching fakebridge.py's convention
            ch.lamp_id = std::to_string(static_cast<unsigned>(data[o]) + 1);
            ch.a = be16(data + o + 1);
            ch.b = be16(data + o + 3);
            ch.c = be16(data + o + 5);
            out.channels.push_back(ch);
        }
    } else {
        return false;
    }

    // Reject duplicate lamp ids within one frame (mirrors fakebridge.py).
    for (size_t i = 0; i < out.channels.size(); ++i) {
        for (size_t j = i + 1; j < out.channels.size(); ++j) {
            if (out.channels[i].lamp_id == out.channels[j].lamp_id) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace huestream
