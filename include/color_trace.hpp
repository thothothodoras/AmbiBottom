#pragma once
// v0.35: optional per-frame colour trace (debug only; off unless --color-trace FILE).
// One JSON line per HueStream frame: every lamp's raw HueStream values (x, y, bri
// as 16-bit numbers, or R, G, B for the RGB colour space), the RGB the bridge made
// of them (display sRGB, then the linear LED value), the JointSpace "processed"
// side colours the bridge had at that moment, and the exact DDP payload bytes (one
// R, G, B byte per LED, in the order they go on the wire). Stops by itself after
// max_seconds so a forgotten flag cannot fill the disk.
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "color_pipeline.hpp"

namespace color_trace {

struct Lamp {
    std::string id;
    uint16_t raw[3] = {0, 0, 0};  // x16, y16, bri16 (colour space 1) or R16, G16, B16 (0)
    color_pipeline::RGB8 rgb;     // what the bridge made of it (display sRGB for XY frames)
    color_pipeline::RGB8 led;     // linear LED value the strip renderer starts from
};

struct Rgb3 {
    uint8_t r = 0, g = 0, b = 0;
};

struct Frame {
    unsigned seq = 0;
    unsigned colorspace = 0;
    std::vector<Lamp> lamps;
    double js_age_s = -1.0;  // -1 = JointSpace never answered
    std::vector<Rgb3> js_left, js_top, js_right;
    std::vector<uint8_t> ddp_payload;  // the LED bytes of the DDP packet(s), header left out
    bool ddp_sent = false;             // false: no --wled-ip, or the send failed
    double estimated_ma = 0.0;
};

class Trace {
public:
    ~Trace();
    // false (and a log line) if the file cannot be opened. max_seconds <= 0 = 600.
    bool open(const std::string& path, double max_seconds);
    bool active();                 // open and not past its time; closes the file once past
    void write(const Frame& f);    // no-op when not active
    const std::string& path() const { return path_; }

    // The JSON line for one frame (exposed for tests).
    static std::string to_json(const Frame& f, double unix_time);

private:
    std::mutex mu_;
    std::FILE* fp_ = nullptr;
    std::string path_;
    double deadline_ = 0.0;  // unix seconds
    unsigned long lines_ = 0;
};

}  // namespace color_trace
