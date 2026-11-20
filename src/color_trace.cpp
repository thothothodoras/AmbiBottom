#include "color_trace.hpp"

#include <chrono>
#include <cstring>

namespace color_trace {
namespace {

double unix_now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void add_rgb(std::string& s, uint8_t r, uint8_t g, uint8_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "[%u,%u,%u]", r, g, b);
    s += buf;
}

void add_zones(std::string& s, const char* name, const std::vector<Rgb3>& z) {
    s += '"';
    s += name;
    s += "\":[";
    for (size_t i = 0; i < z.size(); ++i) {
        if (i) s += ',';
        add_rgb(s, z[i].r, z[i].g, z[i].b);
    }
    s += ']';
}

}  // namespace

Trace::~Trace() {
    std::lock_guard<std::mutex> lk(mu_);
    if (fp_) std::fclose(fp_);
}

bool Trace::open(const std::string& path, double max_seconds) {
    std::lock_guard<std::mutex> lk(mu_);
    if (fp_) std::fclose(fp_);
    fp_ = std::fopen(path.c_str(), "w");
    if (!fp_) {
        std::fprintf(stderr, "[trace] cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }
    path_ = path;
    if (max_seconds <= 0.0) max_seconds = 600.0;
    deadline_ = unix_now() + max_seconds;
    std::fprintf(stderr, "[trace] colour trace to %s for %.0f s (debug; one line per HueStream frame)\n",
                 path.c_str(), max_seconds);
    return true;
}

bool Trace::active() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fp_) return false;
    if (unix_now() > deadline_) {
        std::fclose(fp_);
        fp_ = nullptr;
        std::fprintf(stderr, "[trace] colour trace finished: %lu frames in %s\n", lines_, path_.c_str());
        return false;
    }
    return true;
}

std::string Trace::to_json(const Frame& f, double unix_time) {
    std::string s;
    s.reserve(256 + f.lamps.size() * 96 + f.ddp_payload.size() * 2);
    char buf[160];
    std::snprintf(buf, sizeof buf, "{\"t\":%.3f,\"seq\":%u,\"cs\":%u,\"lamps\":[", unix_time, f.seq, f.colorspace);
    s += buf;
    for (size_t i = 0; i < f.lamps.size(); ++i) {
        const Lamp& l = f.lamps[i];
        if (i) s += ',';
        s += "{\"id\":\"" + l.id + "\",\"raw\":[";
        std::snprintf(buf, sizeof buf, "%u,%u,%u]", l.raw[0], l.raw[1], l.raw[2]);
        s += buf;
        if (f.colorspace == 1) {
            std::snprintf(buf, sizeof buf, ",\"x\":%.4f,\"y\":%.4f,\"bri16\":%u", l.raw[0] / 65535.0,
                          l.raw[1] / 65535.0, l.raw[2]);
            s += buf;
        }
        s += ",\"rgb\":";
        add_rgb(s, l.rgb.r, l.rgb.g, l.rgb.b);
        s += ",\"led\":";
        add_rgb(s, l.led.r, l.led.g, l.led.b);
        s += '}';
    }
    std::snprintf(buf, sizeof buf, "],\"js\":{\"age_s\":%.2f,", f.js_age_s);
    s += buf;
    add_zones(s, "left", f.js_left);
    s += ',';
    add_zones(s, "top", f.js_top);
    s += ',';
    add_zones(s, "right", f.js_right);
    s += "},\"ddp\":\"";
    static const char* hex = "0123456789abcdef";
    for (uint8_t b : f.ddp_payload) {
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    std::snprintf(buf, sizeof buf, "\",\"ddp_sent\":%s,\"ma\":%.1f}\n", f.ddp_sent ? "true" : "false",
                  f.estimated_ma);
    s += buf;
    return s;
}

void Trace::write(const Frame& f) {
    const double now = unix_now();
    const std::string line = to_json(f, now);
    std::lock_guard<std::mutex> lk(mu_);
    if (!fp_) return;
    std::fwrite(line.data(), 1, line.size(), fp_);
    if (++lines_ % 25 == 0) std::fflush(fp_);
}

}  // namespace color_trace
