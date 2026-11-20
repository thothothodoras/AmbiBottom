#include "stream_renderer.hpp"

#include <algorithm>
#include <cstdlib>

namespace color_pipeline {

bool StreamRenderer::on_frame(huestream::Frame frame, const Settings& s, double now_s, const Corners* corners,
                              const std::vector<double>* wear) {
    if (frame.channels.empty()) return false;
    std::sort(frame.channels.begin(), frame.channels.end(), [](const huestream::Channel& a, const huestream::Channel& b) {
        return std::atoi(a.lamp_id.c_str()) < std::atoi(b.lamp_id.c_str());
    });
    std::vector<RGB8> lamps;
    lamps.reserve(frame.channels.size());
    for (const auto& ch : frame.channels) {
        if (frame.colorspace == 1) {
            const double x = ch.a / 65535.0, y = ch.b / 65535.0, bri = ch.c / 65535.0 * 254.0;
            lamps.push_back(srgb_to_led(xy_bri_to_rgb(x, y, bri)));  // display sRGB -> linear LED level
        } else {
            lamps.push_back({rgb16_to_8(ch.a), rgb16_to_8(ch.b), rgb16_to_8(ch.c)});  // already levels
        }
    }
    if (!started_) {
        started_ = true;
        start_ = now_s;
    }
    // Back after a gap: rise from what the strip shows now, one frame step.
    if (have_frame_ && phase_ != "live" && phase_ != "hold") last_out_ = now_s - 0.040;
    if (!have_frame_) last_out_ = now_s - 0.040;
    if (force_restart_) {  // v0.42: Start - soft start from black
        force_restart_ = false;
        last_out_ = now_s - 10.0;
    }
    lamp_count_ = static_cast<int>(lamps.size());
    target_ = render_strip(lamps, s, &info_, corners, now_s - start_, wear);
    const double dt_ms = (now_s - last_out_) * 1000.0;
    {
        const std::vector<RGB8> shown = out_;
        out_ = rise_from_shown(rise_, shown, target_, dt_ms, s.rise_ms, s.fade_in_ms);  // v0.33: also ramps the first frame and after a pause
        if (rise_.remaining_ms <= 0.0) out_ = smooth_small_changes(shown, out_, dt_ms, s);  // v0.44 (not during the soft start)
        out_ = fade_to_dark(dark_, shown, out_, dt_ms, s.fade_out_ms);  // v0.42: picture went dark -> soft fade
    }
    ma_ = estimate_current_ma(out_, s);
    last_out_ = now_s;
    last_frame_ = now_s;
    have_frame_ = true;
    phase_ = "live";
    ++frames_;
    return true;
}

void StreamRenderer::skip_hold(double now_s, const Settings& s) {
    if (!have_frame_ || (phase_ != "live" && phase_ != "hold")) return;
    last_frame_ = std::min(last_frame_, now_s - std::max(0.0, s.loss_hold_ms) / 1000.0);
}

void StreamRenderer::restart_from_black() {
    for (auto& c : out_) c = RGB8{0, 0, 0};
    dark_.active = false;
    force_restart_ = true;
}

bool StreamRenderer::tick(const Settings& s, double now_s) {
    if (!have_frame_ || out_.empty()) return false;
    const double gap_s = now_s - last_frame_;
    if (phase_ == "live") {
        if (gap_s < live_gap_s) return false;
        held_ = out_;
        phase_ = "hold";
        last_repeat_ = now_s - repeat_s;  // show the held frame once right away
    }
    if (phase_ == "stopped") return false;
    const LossOutput lo = stream_loss_output(s, gap_s * 1000.0);
    const std::string phase = !lo.send ? "stopped" : lo.factor >= 1.0 ? "hold" : lo.factor > 0.0 ? "fade" : "black";
    const bool changed = phase != phase_;
    phase_ = phase;
    if (phase == "stopped") return changed;  // the caller lets go of the strip
    if (phase != "fade" && !changed && now_s - last_repeat_ < repeat_s) return false;
    out_ = fit_budget(scale_frame(held_, lo.factor), s);  // v0.33: the held frame obeys the budget of now
    ma_ = estimate_current_ma(out_, s);
    last_out_ = now_s;
    last_repeat_ = now_s;
    return true;
}

}  // namespace color_pipeline
