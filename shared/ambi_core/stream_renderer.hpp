// HueStream frames in, LED values out: the order of steps the Linux bridge
// has used since v0.24 (main.cpp on_frame + output loop), as one small class
// with the clock passed in:
//   lamps sorted by lamp id -> XY+bri to sRGB to linear (RGB frames are taken
//   as levels) -> render_strip (layout, brightness, current limit, peaks) ->
//   limit_rise; and when frames stop: hold -> fade -> black -> stopped
//   (stream_loss_output).
// The ESP32 usermod uses it; C++17 standard library only.
// (The Linux main.cpp still has its own copy of this sequence, woven into the
// monitor snapshots - moving it over to this class is a later step.)
#pragma once
#include <string>
#include <vector>

#include "color_pipeline.hpp"
#include "huestream.hpp"

namespace color_pipeline {

class StreamRenderer {
public:
    // A decoded frame at time now_s (any monotonic clock, seconds). False
    // when the frame has no lamps (nothing changes).
    // v0.41: wear = per-LED wear weights for the wear-aware limiter (Settings::wear_strength), or null.
    bool on_frame(huestream::Frame frame, const Settings& s, double now_s, const Corners* corners = nullptr,
                  const std::vector<double>* wear = nullptr);
    // Call every 20-50 ms. True when leds() changed because of a stream gap
    // (hold / fade / black) and should be shown again.
    bool tick(const Settings& s, double now_s);

    const std::vector<RGB8>& leds() const { return out_; }
    // v0.42: the stream was ended on purpose (stream.active=false): no hold, the fade starts now.
    void skip_hold(double now_s, const Settings& s);
    // v0.42: the strip was given to us just now (Start): the next frame starts a soft start
    // (fade_in_ms) from black, not from what was computed while the strip was WLED's.
    void restart_from_black();
    // "idle" (no frame yet) | "live" | "hold" | "fade" | "black" | "stopped"
    const std::string& phase() const { return phase_; }
    // True while the strip should be driven by us (live .. black).
    bool driving() const { return phase_ != "idle" && phase_ != "stopped"; }
    double last_frame_time() const { return have_frame_ ? last_frame_ : -1.0; }
    double current_ma() const { return ma_; }
    const RenderInfo& info() const { return info_; }
    int lamp_count() const { return lamp_count_; }
    unsigned long frames() const { return frames_; }

    // Frames with less than this gap are "flowing" (25 fps = 40 ms).
    static constexpr double live_gap_s = 0.150;
    // During hold / black the same values are repeated this often.
    static constexpr double repeat_s = 0.500;

private:
    std::vector<RGB8> out_, target_, held_;
    std::string phase_ = "idle";
    bool have_frame_ = false;
    double last_frame_ = 0.0, last_out_ = 0.0, last_repeat_ = 0.0, start_ = 0.0;
    bool started_ = false;
    double ma_ = 0.0;
    RenderInfo info_;
    RiseState rise_;  // v0.33: soft start after a start / pause
    DarkFadeState dark_;  // v0.42: soft fade when the picture goes dark
    bool force_restart_ = false;
    int lamp_count_ = 0;
    unsigned long frames_ = 0;
};

}  // namespace color_pipeline
