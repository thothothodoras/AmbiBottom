// Shared thread-safe snapshot of "what the bridge knows right now", built for
// the monitor web page (monitor_server.hpp). Two independent producers feed
// this: main.cpp's on_frame handler (the Hue Entertainment/DTLS side - the
// same data it already computes for the terminal preview) and the JointSpace
// polling thread (jointspace_client.hpp - the TV's own screen-capture zones,
// read-only, over HTTPS+Digest on port 1926). This is just a second,
// browser-facing presentation of both, not a second data path for either.
//
// Mirrors the field semantics of the old Python prototype's
// ambilight-monitor/1 JSON contract (docs/contract.md in the old project)
// closely enough that the ported web page's JS needed almost no changes:
// raw values are stored here unconditionally (last known), and "is this
// actually live right now" is computed at snapshot time from timestamps -
// exactly like the old snapshot_from_fakebridge() did - rather than storing
// a stale "is_live" flag that could itself go stale.
#pragma once
#include "jointspace_fetch.hpp"
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "color_pipeline.hpp"
#include "tv_lock.hpp"

namespace monitor_state {

struct LampSnapshot {
    std::string name;
    color_pipeline::RGB8 rgb{0, 0, 0};
    bool has_xy = false;
    double x = 0, y = 0;
    bool has_bri8 = false;
    double bri8 = 0;
    uint16_t raw16[3] = {0, 0, 0};
};

// v0.13: how the strip was limited (from color_pipeline::RenderInfo +
// the settings it used), for the monitor.
struct StripInfo {
    double wear_strength = 0.0;  // v0.33
    bool wear_applied = false;   // v0.33: the wear weights took part in this frame's limiting
    double budget_ma = 0.0;
    double ma_per_led_full_white = 0.0;
    double peak_emphasis = 0.0;
    double rise_ms = 0.0;
    double lamp_inset = 0.0;  // v0.16, Settings::lamp_inset of this frame
    std::string blend_mode = "smooth", corner_mode = "lamp";  // v0.18
    double blend_softness = 0.0;    // v0.23 (NEXT 9)
    double peak_drift_leds = 0.0;   // v0.23 (NEXT 5): setting
    double peak_offset_leds = 0.0;  // v0.23: drift applied on this frame
    // v0.24: drift amplitude actually used (resolves "auto"), auto flag, shape
    bool peak_drift_auto = false;
    std::string peak_drift_shape = "triangle";
    bool end_lamps = false;          // v0.24: end_lamps in effect on this frame
    double corner_weight = 1.0;      // v0.24: JointSpace corner share 0..1 (fades when stale)
    // v0.30: strip layout "tv" | "inset"; edge colour (tv layout): LEDs,
    // strength, source in effect ("lamps" | "jointspace" | "none") and the setting
    std::string strip_layout = "tv";
    int edge_leds = 0;
    double edge_strength = 0.0;
    std::string edge_source = "none", edge_source_setting = "auto";
    // v0.24 (review item 1): "live" | "hold" | "fade" | "black" | "stopped"
    std::string output_phase = "live";
    double wanted_ma = 0.0;         // v0.23: what the frame asked for before the limit
    std::string limit_mode = "none";
    double peak_scale = 1.0, min_scale = 1.0;
    std::vector<int> peak_leds;
    double target_ma = 0.0;  // current of the limited target, before the rise limit
    // The same frame rendered with peak_emphasis = 0 (the pre-v0.13
    // limiter), only for the page's "Σύγκριση pe=0" view. Not sent anywhere.
    std::vector<color_pipeline::RGB8> compare_pe0;
    double compare_pe0_ma = 0.0;
};

// v0.37: the JointSpace zone type lives in the shared core (shared/ambi_core/jointspace_fetch.hpp)
using Zone = jointspace::Zone;

// Everything the monitor page's /api/state response needs, already resolved
// against "now" by get() - callers never have to re-derive liveness.
struct Snapshot {
    // ---- Hue Entertainment / DTLS side -------------------------------
    std::vector<LampSnapshot> lamps;  // last known value per bottom lamp
    bool lamps_good = false;          // true => lamps[] above is live, not stale
    int used_anchors = 0;
    double estimated_ma = 0.0;
    uint32_t seq = 0;
    bool stream_active = false;
    double hue_age_s = -1.0;      // -1 = no frame received yet this run
    double hue_fps = 0.0;
    uint32_t hue_frames = 0;
    uint32_t hue_bad_frames = 0;
    std::string hue_version;      // "1.0" / "2.0", empty = unknown yet
    int hue_colorspace = -1;      // 0 = RGB, 1 = XY+bri, -1 = unknown yet
    // v0.21: a client streams from a session from before this bridge's
    // restart (see dtls_psk::OldSessionHandler). Active = still arriving.
    bool old_session = false;
    std::string old_session_ip;
    uint64_t old_session_packets = 0;
    // v0.31: the TV asked for the stream (stream.active=true) but no DTLS
    // handshake completed within its own 3 s limit, or the handshake failed.
    // The TV then stays on LightGroup (no stream) until its Hue process
    // restarts - in practice a TV restart. Cleared by the next good handshake.
    bool tv_dtls_lock = false;
    std::string tv_dtls_lock_reason;     // "timeout" | "failed"
    std::string tv_dtls_lock_detail;     // error text for "failed"
    double tv_dtls_lock_age_s = 0.0;
    bool tv_dtls_lock_confirmed = false; // the TV created/used a LightGroup afterwards
    double last_handshake_latency_s = -1.0;  // stream.active=true -> handshake done; -1 = none yet

    // ---- JointSpace side (TV's own left/top/right screen-capture) ----
    std::string js_status;   // "not_configured" | "live" | "unavailable"
    std::string js_error;    // transport error code, or empty
    double js_age_s = -1.0;
    double js_fps = 0.0;
    std::vector<Zone> js_left, js_top, js_right;  // [] when not live

    // v0.12: the bottom strip exactly as the bridge computed it (linear
    // LED PWM values, what goes to WLED), only while lamps_good.
    std::vector<color_pipeline::RGB8> strip;
    StripInfo strip_info;
    // v0.12: DDP output to WLED (--wled-ip). Empty target = disabled.
    std::string wled_target;
    double wled_fps = 0.0;
    uint64_t wled_frames = 0;
    uint64_t wled_errors = 0;
    std::string wled_last_error;  // empty = last send was fine
};

// Cheap to copy (a handful of lamps + ~14 zone pixels); callers take
// Snapshot by value.
class SharedState {
public:
    // Called from main.cpp's on_frame for every successfully parsed
    // HueStream frame. `xy`/`bri8` are only meaningful when colorspace==1
    // (caller passes has_xy=false otherwise); `raw16` is always the raw
    // payload regardless of colorspace, same as the old bridge's
    // "values16" diagnostic field.
    void update_frame(const std::vector<LampSnapshot>& lamps, int used_anchors, double estimated_ma,
                       uint32_t seq, uint8_t major, uint8_t minor, int colorspace) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mu_);
        lamps_ = lamps;
        used_anchors_ = used_anchors;
        estimated_ma_ = estimated_ma;
        seq_ = seq;
        hue_version_ = std::to_string(major) + "." + std::to_string(minor);
        hue_colorspace_ = colorspace;
        last_frame_ = now;
        have_frame_ = true;
        ++hue_frames_;
        hue_times_.push_back(now);
        trim_times(hue_times_, now);
    }

    // v0.12: called on every frame with the strip that is (or would be)
    // sent to WLED, plus the settings it was computed with.
    void update_strip(const std::vector<color_pipeline::RGB8>& strip, const StripInfo& info) {
        std::lock_guard<std::mutex> lk(mu_);
        strip_ = strip;
        strip_info_ = info;
    }

    void set_wled_target(const std::string& target) {
        std::lock_guard<std::mutex> lk(mu_);
        wled_target_ = target;
    }

    void note_wled_send(bool ok, const std::string& error) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mu_);
        if (ok) {
            ++wled_frames_;
            wled_last_error_.clear();
            wled_times_.push_back(now);
        } else {
            ++wled_errors_;
            wled_last_error_ = error;
        }
        trim_times(wled_times_, now);
    }

    void note_bad_frame() {
        std::lock_guard<std::mutex> lk(mu_);
        ++hue_frames_;
        ++hue_bad_frames_;
    }

    void set_old_session(const std::string& ip, uint64_t packets, bool active) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = std::chrono::steady_clock::now();
        // v0.37: start of an episode (for the 90 s cap of old_session_nudge)
        if (active && (!old_session_active_ || ip != old_session_ip_ ||
                       std::chrono::duration<double>(now - old_session_seen_).count() > 10.0))
            old_session_since_ = now;
        old_session_active_ = active;
        old_session_ip_ = ip;
        old_session_packets_ = packets;
        old_session_seen_ = std::chrono::steady_clock::now();
    }

    // v0.37: true while `ip` streams from an old DTLS session (packets within the last 3 s;
    // the DTLS server reports every 2 s), for at most 90 s per episode: hue_rest then fails
    // the TV's lamp check so the TV goes to its Reconnection state by itself.
    bool old_session_nudge(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto now = std::chrono::steady_clock::now();
        return old_session_active_ && ip == old_session_ip_ &&
               std::chrono::duration<double>(now - old_session_seen_).count() <= 3.0 &&
               std::chrono::duration<double>(now - old_session_since_).count() <= 90.0;
    }

    void set_stream_active(bool active) {
        std::lock_guard<std::mutex> lk(mu_);
        stream_active_ = active;
    }

    // ---- v0.31: TV locked to LightGroup after a DTLS failure -------------
    // v0.32: the rules live in shared/ambi_core/tv_lock (also used by the ESP32).
    static constexpr double tv_dtls_limit_s = tv_lock::Tracker::default_limit_s;

    void note_stream_requested() {
        std::lock_guard<std::mutex> lk(mu_);
        lock_.stream_requested(mono_s());
    }
    void note_stream_released() {
        std::lock_guard<std::mutex> lk(mu_);
        lock_.stream_released();
    }
    double note_handshake(bool ok, const std::string& detail) {
        std::lock_guard<std::mutex> lk(mu_);
        return lock_.handshake(ok, detail, mono_s());
    }
    bool check_dtls_deadline() {
        std::lock_guard<std::mutex> lk(mu_);
        const double last = have_frame_ ? std::chrono::duration<double>(last_frame_.time_since_epoch()).count() : -1.0;
        return lock_.check_deadline(mono_s(), last);
    }
    void note_lightgroup_used() {
        std::lock_guard<std::mutex> lk(mu_);
        lock_.lightgroup_used();
    }

    void set_lamp_names(const std::vector<std::string>& names) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!lamps_.empty()) return;  // only used before the first real frame
        for (auto& n : names) {
            LampSnapshot l;
            l.name = n;
            lamps_.push_back(l);
        }
    }

    // Called from the JointSpace polling thread (jointspace_client.cpp)
    // after every poll attempt, success or failure. On failure, `left`/
    // `top`/`right` are ignored and the previously-known zones/timestamp
    // are kept (so "age" keeps growing instead of snapping to "now" on a
    // failed poll) - matching the old bridge's js_thread() semantics.
    void set_jointspace_result(bool ok, const std::string& status, const std::string& error,
                                std::vector<Zone> left, std::vector<Zone> top, std::vector<Zone> right) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mu_);
        js_status_ = status;
        js_error_ = error;
        if (ok) {
            js_left_ = std::move(left);
            js_top_ = std::move(top);
            js_right_ = std::move(right);
            last_js_success_ = now;
            have_js_success_ = true;
            js_times_.push_back(now);
            trim_times(js_times_, now);
        }
    }

    Snapshot get() const {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mu_);
        Snapshot s;
        s.lamps = lamps_;
        s.hue_age_s = have_frame_ ? std::chrono::duration<double>(now - last_frame_).count() : -1.0;
        // Same two-part gate as the old bridge: the group must actually be
        // streaming (stream.active=true) AND a real frame must have
        // arrived recently - either one alone isn't "live".
        s.stream_active = stream_active_;
        s.lamps_good = stream_active_ && s.hue_age_s >= 0 && s.hue_age_s <= kHueMaxAgeS;
        s.used_anchors = used_anchors_;
        s.estimated_ma = estimated_ma_;
        s.seq = seq_;
        s.hue_frames = hue_frames_;
        s.hue_bad_frames = hue_bad_frames_;
        s.hue_version = hue_version_;
        s.hue_colorspace = hue_colorspace_;
        s.hue_fps = fps_in_window(hue_times_, now);
        // Reported every ~2 s while old-session packets keep coming; 6 s
        // of silence = it stopped (TV gave up or was turned off).
        s.old_session = old_session_active_ &&
                        std::chrono::duration<double>(now - old_session_seen_).count() <= 6.0 && !s.lamps_good;
        s.old_session_ip = old_session_ip_;
        s.old_session_packets = old_session_packets_;
        {
            const tv_lock::Status ls = lock_.status(mono_s());
            s.tv_dtls_lock = ls.locked;
            s.tv_dtls_lock_reason = ls.reason;
            s.tv_dtls_lock_detail = ls.detail;
            s.tv_dtls_lock_age_s = ls.age_s;
            s.tv_dtls_lock_confirmed = ls.confirmed;
            s.last_handshake_latency_s = ls.last_handshake_latency_s;
        }

        s.js_status = js_status_;
        s.js_error = js_error_;
        s.js_age_s = have_js_success_ ? std::chrono::duration<double>(now - last_js_success_).count() : -1.0;
        s.js_fps = fps_in_window(js_times_, now);
        if (s.lamps_good) s.strip = strip_;
        s.strip_info = strip_info_;
        s.wled_target = wled_target_;
        s.wled_fps = fps_in_window(wled_times_, now);
        s.wled_frames = wled_frames_;
        s.wled_errors = wled_errors_;
        s.wled_last_error = wled_last_error_;
        bool js_live = js_status_ == "live" && s.js_age_s >= 0 && s.js_age_s <= kJsMaxAgeS;
        if (js_live) {
            s.js_left = js_left_;
            s.js_top = js_top_;
            s.js_right = js_right_;
        }
        return s;
    }

private:
    static constexpr double kHueMaxAgeS = 1.3;  // same threshold as the old bridge
    static constexpr double kJsMaxAgeS = 2.5;

    static void trim_times(std::deque<std::chrono::steady_clock::time_point>& times,
                            std::chrono::steady_clock::time_point now) {
        while (!times.empty() && std::chrono::duration<double>(now - times.front()).count() > 2.0) times.pop_front();
    }
    static double fps_in_window(const std::deque<std::chrono::steady_clock::time_point>& times,
                                 std::chrono::steady_clock::time_point now) {
        // Count samples still inside the trailing 2s window (trim_times is
        // only called on write, so re-check here for an idle read path).
        size_t n = 0;
        for (auto& t : times) {
            if (std::chrono::duration<double>(now - t).count() <= 2.0) ++n;
        }
        return n / 2.0;
    }

    mutable std::mutex mu_;

    std::vector<LampSnapshot> lamps_;
    int used_anchors_ = 0;
    double estimated_ma_ = 0.0;
    uint32_t seq_ = 0;
    bool stream_active_ = false;
    std::chrono::steady_clock::time_point last_frame_;
    bool have_frame_ = false;
    uint32_t hue_frames_ = 0, hue_bad_frames_ = 0;
    std::string hue_version_;
    int hue_colorspace_ = -1;
    std::deque<std::chrono::steady_clock::time_point> hue_times_;
    std::chrono::steady_clock::time_point old_session_since_;
    bool old_session_active_ = false;
    std::string old_session_ip_;
    uint64_t old_session_packets_ = 0;
    std::chrono::steady_clock::time_point old_session_seen_;
    tv_lock::Tracker lock_;
    static double mono_s() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string js_status_ = "not_configured";
    std::string js_error_;
    std::vector<Zone> js_left_, js_top_, js_right_;
    std::chrono::steady_clock::time_point last_js_success_;
    bool have_js_success_ = false;
    std::deque<std::chrono::steady_clock::time_point> js_times_;

    std::vector<color_pipeline::RGB8> strip_;
    StripInfo strip_info_;
    std::string wled_target_, wled_last_error_;
    uint64_t wled_frames_ = 0, wled_errors_ = 0;
    std::deque<std::chrono::steady_clock::time_point> wled_times_;
};

}  // namespace monitor_state
