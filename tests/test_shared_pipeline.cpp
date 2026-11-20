// Tests of shared/ambi_core that links ONLY ambi_pipeline (colour pipeline,
// HueStream parser, TV-lock rules): proof that this part - the one the ESP32
// usermod compiles too - needs nothing but the C++ standard library.
#include "jointspace_fetch.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "color_pipeline.hpp"
#include "settings.hpp"
#include "huestream.hpp"
#include "stream_renderer.hpp"
#include "tv_lock.hpp"

using color_pipeline::RGB8;
using color_pipeline::Settings;

static void test_tv_lock_rules() {
    tv_lock::Tracker t;
    assert(t.limit_s() == 3.0);
    // the TV's normal order: request, handshake 0.02 s later
    t.stream_requested(100.0);
    assert(!t.check_deadline(100.5, -1));
    double lat = t.handshake(true, "", 100.02);
    assert(std::fabs(lat - 0.02) < 1e-9);
    assert(!t.status(101).locked && std::fabs(t.status(101).last_handshake_latency_s - 0.02) < 1e-9);
    assert(!t.check_deadline(200.0, -1));  // nothing awaited any more

    // request and nothing: lock by timeout, reported exactly once
    t.stream_requested(300.0);
    assert(!t.check_deadline(302.9, -1));
    assert(t.check_deadline(303.0, -1));
    assert(!t.check_deadline(304.0, -1));
    tv_lock::Status s = t.status(363.0);
    assert(s.locked && s.reason == "timeout" && !s.confirmed && std::fabs(s.age_s - 60.0) < 1e-9);
    t.lightgroup_used();
    assert(t.status(364.0).confirmed);
    // a good handshake clears it (the TV was restarted)
    assert(t.handshake(true, "", 400.0) < 0);  // not awaited: no latency
    assert(!t.status(401).locked && !t.status(401).confirmed);

    // failed handshake while waiting: lock "failed" with the reason text
    t.stream_requested(500.0);
    t.handshake(false, "bad MAC", 500.4);
    s = t.status(500.5);
    assert(s.locked && s.reason == "failed" && s.detail == "bad MAC");
    // a failed handshake nobody asked for does not lock
    tv_lock::Tracker u;
    u.handshake(false, "noise", 1.0);
    assert(!u.status(2.0).locked);
    // lightgroup without a lock is nothing
    u.lightgroup_used();
    assert(!u.status(2.0).confirmed);

    // frames since the request = a session was already up: no lock
    tv_lock::Tracker v;
    v.stream_requested(10.0);
    assert(!v.check_deadline(13.5, 12.0));
    assert(!v.status(14).locked);
    // released before the limit: no lock
    v.stream_requested(20.0);
    v.stream_released();
    assert(!v.check_deadline(30.0, -1));
    // the limit is a setting
    tv_lock::Tracker w(1.0);
    w.stream_requested(0.0);
    assert(w.check_deadline(1.0, -1));
    std::printf("[ok] tv_lock: wait / timeout / failed / confirmed / cleared / frames-seen / limit setting\n");
}

static bool same(const RGB8& a, const RGB8& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static void test_brightness() {
    Settings s;
    s.led_count = 30;
    s.ma_per_led_full_white = 1.0;  // v0.33: no limiting in this test (the budget itself has a ceiling)
    s.idle_ma_per_led = 0.0;
    s.rise_ms = 0;
    const std::vector<RGB8> lamps = {{200, 100, 50}, {200, 100, 50}, {200, 100, 50}};
    const std::vector<RGB8> full = color_pipeline::render_strip(lamps, s);
    s.brightness = 1.0;
    const std::vector<RGB8> one = color_pipeline::render_strip(lamps, s);
    for (size_t i = 0; i < full.size(); ++i) assert(same(full[i], one[i]));  // 1.0 = unchanged
    s.brightness = 0.5;
    const std::vector<RGB8> half = color_pipeline::render_strip(lamps, s);
    for (size_t i = 0; i < full.size(); ++i) {
        assert(std::abs(int(half[i].r) - int(full[i].r) / 2) <= 1);
        assert(std::abs(int(half[i].g) - int(full[i].g) / 2) <= 1);
        assert(std::abs(int(half[i].b) - int(full[i].b) / 2) <= 1);
    }
    s.brightness = 0.0;
    for (const RGB8& c : color_pipeline::render_strip(lamps, s)) assert(c.r == 0 && c.g == 0 && c.b == 0);

    // brightness comes BEFORE the limit: at 0.25 a white frame that was over
    // budget is under it, and the limiter leaves it alone
    Settings t;
    t.led_count = 43;
    t.rise_ms = 0;
    assert(t.current_budget_ma == 450.0);  // v0.32 default
    const std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    color_pipeline::RenderInfo info;
    std::vector<RGB8> out = color_pipeline::render_strip(white, t, &info, nullptr, 0.0);
    assert(info.wanted_ma > t.current_budget_ma);
    assert(color_pipeline::estimate_current_ma(out, t) <= t.current_budget_ma + 1e-6);  // limit holds
    t.brightness = 0.1;
    out = color_pipeline::render_strip(white, t, &info, nullptr, 0.0);
    assert(info.wanted_ma <= t.current_budget_ma);
    assert(color_pipeline::estimate_current_ma(out, t) <= t.current_budget_ma + 1e-6);
    std::printf("[ok] brightness: 1.0 unchanged, 0.5 halves, 0 black, applied before the current limit (default 450 mA)\n");
}

static void test_custom_lamp_positions() {
    Settings s;
    s.led_count = 41;
    s.ma_per_led_full_white = 1.0;  // v0.33: no limiting
    s.idle_ma_per_led = 0.0;
    s.rise_ms = 0;
    s.blend_mode = color_pipeline::kBlendZones;
    s.edge_leds = 0;
    const RGB8 R{255, 0, 0}, G{0, 255, 0}, B{0, 0, 255};
    const std::vector<RGB8> lamps = {R, G, B};

    // not custom: lamp 1 at the start, lamp 3 at the end
    std::vector<RGB8> tv = color_pipeline::render_strip(lamps, s);
    assert(same(tv.front(), R) && same(tv.back(), B));

    // custom, reversed: lamp 1 at the END
    s.strip_layout = color_pipeline::kLayoutCustom;
    s.lamp_positions = {1.0, 0.5, 0.0};
    assert(color_pipeline::custom_layout_active(s, 3));
    std::vector<RGB8> rev = color_pipeline::render_strip(lamps, s);
    assert(same(rev.front(), B) && same(rev[20], G) && same(rev.back(), R));

    // a lamp left out (negative) is not shown anywhere
    s.lamp_positions = {0.0, -1.0, 1.0};
    for (const RGB8& c : color_pipeline::render_strip(lamps, s)) assert(!same(c, G));
    const std::vector<double> pos = color_pipeline::strip_lamp_positions(s, 3);
    assert(pos.size() == 2 && pos[0] == 0.0 && pos[1] == 1.0);

    // wrong length for this stream, or nothing shown: falls back to "tv"
    s.lamp_positions = {0.0, 1.0};
    assert(!color_pipeline::custom_layout_active(s, 3));
    std::vector<RGB8> fb = color_pipeline::render_strip(lamps, s);
    for (size_t i = 0; i < fb.size(); ++i) assert(same(fb[i], tv[i]));
    s.lamp_positions = {-1.0, -1.0, -1.0};
    assert(!color_pipeline::custom_layout_active(s, 3));

    assert(std::string(color_pipeline::strip_layout_name(color_pipeline::kLayoutCustom)) == "custom");
    assert(color_pipeline::strip_layout_from("custom", 0) == color_pipeline::kLayoutCustom);
    std::printf("[ok] custom lamp positions: reorder, leave a lamp out, fall back to tv when they do not fit\n");
}

static void test_stream_to_strip() {
    // one HueStream v1 frame, 3 lamps in XY+bri, through the whole pipeline
    std::vector<uint8_t> f = {'H', 'u', 'e', 'S', 't', 'r', 'e', 'a', 'm', 1, 0, 7, 0, 0, 1, 0};
    auto add = [&](uint8_t id, uint16_t a, uint16_t b, uint16_t c) {
        f.insert(f.end(), {0, 0, id, uint8_t(a >> 8), uint8_t(a), uint8_t(b >> 8), uint8_t(b), uint8_t(c >> 8), uint8_t(c)});
    };
    add(1, 20000, 20000, 65535);
    add(2, 20000, 20000, 30000);
    add(3, 20000, 20000, 0);
    huestream::Frame fr;
    assert(huestream::parse_frame(f, fr));
    assert(fr.channels.size() == 3 && fr.colorspace == 1 && fr.seq == 7);
    std::vector<RGB8> lamps;
    for (const auto& ch : fr.channels)
        lamps.push_back(color_pipeline::srgb_to_led(
            color_pipeline::xy_bri_to_rgb(ch.a / 65535.0, ch.b / 65535.0, ch.c / 65535.0 * 254.0)));
    Settings s;
    s.led_count = 20;
    const std::vector<RGB8> strip = color_pipeline::render_strip(lamps, s);
    assert(strip.size() == 20);
    assert(strip.front().r + strip.front().g + strip.front().b > strip.back().r + strip.back().g + strip.back().b);
    assert(color_pipeline::estimate_current_ma(strip, s) <= s.current_budget_ma + 1e-6);
    assert(!huestream::parse_frame(f.data(), 10, fr));  // truncated header
    std::printf("[ok] HueStream frame -> lamps -> 20 LEDs, bright end to dark end, within the budget\n");
}

static huestream::Frame xy_frame(uint16_t bri1, uint16_t bri2, uint16_t bri3) {
    huestream::Frame f;
    f.major = 1;
    f.colorspace = 1;
    // deliberately out of order: lamp 3 first
    f.channels = {{"3", 20000, 20000, bri3}, {"1", 20000, 20000, bri1}, {"2", 20000, 20000, bri2}};
    return f;
}

// v0.42: soft start over fade_in_ms; the picture going dark fades over fade_out_ms; normal falls stay immediate;
// skip_hold (stream.active=false) starts the loss fade at once; restart_from_black (Start) ramps again.
// v0.48: white balance - per-channel gains with the brightness, before the current limit
static void test_white_balance() {
    using color_pipeline::RGB8;
    Settings s;
    s.led_count = 6;
    s.current_budget_ma = 100000;
    s.peak_emphasis = 0;
    const std::vector<RGB8> lamps(3, RGB8{200, 200, 200});
    const auto plain = color_pipeline::render_strip(lamps, s);
    s.wb_b = 0.5;
    s.wb_g = 0.9;
    const auto warm = color_pipeline::render_strip(lamps, s);
    assert(plain.size() == warm.size());
    for (size_t i = 0; i < warm.size(); ++i) {
        assert(warm[i].r == plain[i].r);
        assert(std::abs(int(warm[i].b) - int(plain[i].b) / 2) <= 1);
        assert(std::abs(int(warm[i].g) - int(plain[i].g * 9 / 10)) <= 1);
    }
    s.wb_b = 0.0;  // out of range -> sanitize clamps to 0.2
    settings::sanitize(s);
    assert(s.wb_b == 0.2);
    std::printf("[ok] v0.48 white balance: blue x0.5, green x0.9, red kept; clamped to 0.2..1\n");
    // v0.49: keep pure colours - a pure blue is not dimmed, the TV's bluish white is corrected in full
    Settings k;
    k.led_count = 6;
    k.current_budget_ma = 100000;
    k.peak_emphasis = 0;
    k.wb_b = 0.5;
    k.wb_keep_pure = true;
    const auto blue_plain = color_pipeline::render_strip(std::vector<RGB8>(3, RGB8{0, 0, 200}), [&] { Settings z = k; z.wb_b = 1; return z; }());
    const auto blue_kept = color_pipeline::render_strip(std::vector<RGB8>(3, RGB8{0, 0, 200}), k);
    assert(blue_kept[2].b == blue_plain[2].b);
    const auto tvw_plain = color_pipeline::render_strip(std::vector<RGB8>(3, RGB8{110, 135, 215}), [&] { Settings z = k; z.wb_b = 1; return z; }());
    const auto tvw = color_pipeline::render_strip(std::vector<RGB8>(3, RGB8{110, 135, 215}), k);
    assert(std::abs(int(tvw[2].b) - int(tvw_plain[2].b) / 2) <= 1);  // saturation ~0.49: full correction
    k.wb_keep_pure = false;
    const auto blue_all = color_pipeline::render_strip(std::vector<RGB8>(3, RGB8{0, 0, 200}), k);
    assert(std::abs(int(blue_all[2].b) - int(blue_plain[2].b) / 2) <= 1);
    std::printf("[ok] v0.49 keep pure colours: pure blue kept, the TV's bluish white corrected, off = v0.48\n");
    // v0.49: the board shares the strip's port -> the strip gets the port limit minus its share
    Settings pw;
    pw.led_count = 39;
    pw.ma_per_led_full_white = 40;
    pw.current_budget_ma = 810;
    pw.esp_same_port_ma = 120;
    assert(color_pipeline::effective_budget_ma(pw) == 690);
    const auto full = color_pipeline::render_strip(std::vector<RGB8>(3, RGB8{255, 255, 255}), pw);
    assert(color_pipeline::estimate_current_ma(full, pw) <= 690 + 1e-6);
    pw.esp_same_port_ma = 0;
    assert(color_pipeline::effective_budget_ma(pw) == 810);
    pw.current_budget_ma = 100;
    pw.esp_same_port_ma = 400;
    assert(color_pipeline::effective_budget_ma(pw) == 50);  // never below 50 mA for the strip
    std::printf("[ok] v0.49 shared USB port: strip budget = port limit - the board's share (>= 50 mA)\n");
}

// v0.45: the JointSpace pairing signature = base64(HMAC-SHA1(shared key, timestamp + pin)),
// value computed independently with Python's hmac module
static void test_js_pair_signature() {
    assert(jointspace::pair_signature("1700000000", "1234") == "Up43OnIb8sG9V66+HISRwCPnYns=");
    assert(jointspace::pair_signature("1700000000", "1235") != jointspace::pair_signature("1700000000", "1234"));
    std::printf("[ok] v0.45 JointSpace pairing signature matches the reference value\n");
}

// v0.44: adaptive smoothing - small changes are eased, a scene cut passes at once, the budget holds
static void test_smoothing() {
    using color_pipeline::RGB8;
    Settings s;
    s.led_count = 4;
    s.smooth_ms = 150;
    s.current_budget_ma = 10000;
    const std::vector<RGB8> a(4, RGB8{100, 100, 100});
    // a small step (+10): after one 40 ms frame only part of the way, after ~0.6 s there
    std::vector<RGB8> small(4, RGB8{110, 110, 110});
    auto o = color_pipeline::smooth_small_changes(a, small, 40, s);
    assert(o[0].r > 100 && o[0].r < 110);
    for (int i = 0; i < 15; ++i) o = color_pipeline::smooth_small_changes(o, small, 40, s);
    assert(o[0].r == 110);
    // a scene cut (+120): at once
    std::vector<RGB8> cut(4, RGB8{220, 220, 220});
    o = color_pipeline::smooth_small_changes(a, cut, 40, s);
    assert(o[0].r == 220);
    // a wobble 100 -> 104 -> 100 stays close to 100
    o = color_pipeline::smooth_small_changes(a, std::vector<RGB8>(4, RGB8{104, 104, 104}), 40, s);
    o = color_pipeline::smooth_small_changes(o, a, 40, s);
    assert(std::abs(int(o[0].r) - 100) <= 2);
    // off / pause / size change -> unchanged
    Settings off = s;
    off.smooth_ms = 0;
    assert(color_pipeline::smooth_small_changes(a, small, 40, off)[0].r == 110);
    assert(color_pipeline::smooth_small_changes(a, small, 500, s)[0].r == 110);
    // the budget holds
    Settings tight = s;
    tight.current_budget_ma = 100;
    const auto t = color_pipeline::smooth_small_changes(std::vector<RGB8>(4, RGB8{200, 0, 0}), std::vector<RGB8>(4, RGB8{0, 200, 0}), 40, tight);
    assert(color_pipeline::estimate_current_ma(t, tight) <= tight.current_budget_ma + 1e-6);
    std::printf("[ok] v0.44 smoothing: small steps eased, a cut passes at once, wobbles damped, off / pause unchanged, budget holds\n");
}

static void test_soft_on_off() {
    using color_pipeline::RGB8;
    Settings s;
    s.led_count = 10;
    s.rise_ms = 120;
    s.fade_in_ms = 800;
    s.fade_out_ms = 1500;
    s.current_budget_ma = 2000;
    color_pipeline::StreamRenderer r;
    double t = 100.0;
    // soft start: after 200 ms (5 frames) clearly below the target, after ~1 s at the target
    for (int i = 0; i < 5; ++i, t += 0.04) r.on_frame(xy_frame(40000, 40000, 40000), s, t);
    int sum5 = 0;
    for (const auto& c : r.leds()) sum5 += c.r + c.g + c.b;
    for (int i = 0; i < 25; ++i, t += 0.04) r.on_frame(xy_frame(40000, 40000, 40000), s, t);
    int full = 0;
    for (const auto& c : r.leds()) full += c.r + c.g + c.b;
    assert(full > 0 && sum5 < full * 0.6);
    // the picture goes dark: still lit after 0.5 s, dark after 1.6 s
    for (int i = 0; i < 12; ++i, t += 0.04) r.on_frame(xy_frame(0, 0, 0), s, t);
    int mid = 0;
    for (const auto& c : r.leds()) mid += c.r + c.g + c.b;
    assert(mid > 0 && mid < full);
    for (int i = 0; i < 30; ++i, t += 0.04) r.on_frame(xy_frame(0, 0, 0), s, t);
    assert(color_pipeline::is_dark(r.leds()));
    // a normal fall (not to dark) is immediate: bright -> half in one frame step
    for (int i = 0; i < 40; ++i, t += 0.04) r.on_frame(xy_frame(60000, 60000, 60000), s, t);
    int bright = 0;
    for (const auto& c : r.leds()) bright += c.r + c.g + c.b;
    r.on_frame(xy_frame(15000, 15000, 15000), s, t);
    t += 0.04;
    int dimmer = 0;
    for (const auto& c : r.leds()) dimmer += c.r + c.g + c.b;
    assert(dimmer < bright * 0.7);
    // Start: restart_from_black -> the next frame is a soft start again
    r.restart_from_black();
    r.on_frame(xy_frame(60000, 60000, 60000), s, t);
    int after_start = 0;
    for (const auto& c : r.leds()) after_start += c.r + c.g + c.b;
    assert(after_start < bright * 0.3);
    // stream.active=false: skip_hold -> the loss fade starts right away (no 1 s hold)
    for (int i = 0; i < 40; ++i, t += 0.04) r.on_frame(xy_frame(60000, 60000, 60000), s, t);
    r.skip_hold(t, s);
    r.tick(s, t + 0.2);   // live -> hold/fade: with the hold skipped we are already fading
    assert(r.phase() == "fade");
    std::printf("[ok] v0.42 soft start (fade_in_ms), dark fade (fade_out_ms), normal falls immediate, Start ramps again, stream end skips the hold\n");
}

static void test_stream_renderer() {
    Settings s;
    s.led_count = 20;
    s.rise_ms = 0;
    s.fade_in_ms = 0;   // v0.42: no soft start / dark fade in this test (the steps are compared by hand)
    s.fade_out_ms = 0;
    s.smooth_ms = 0;    // v0.44
    s.loss_hold_ms = 1000;
    s.loss_fade_ms = 1000;
    s.loss_black_s = 5;
    color_pipeline::StreamRenderer r;
    assert(r.phase() == "idle" && !r.driving() && !r.tick(s, 0.0) && r.last_frame_time() < 0);
    huestream::Frame empty;
    assert(!r.on_frame(empty, s, 0.0));

    // same result as doing the steps by hand; lamps are sorted by id
    assert(r.on_frame(xy_frame(65535, 30000, 0), s, 10.0));
    std::vector<RGB8> lamps;
    for (uint16_t bri : {uint16_t(65535), uint16_t(30000), uint16_t(0)})
        lamps.push_back(color_pipeline::srgb_to_led(color_pipeline::xy_bri_to_rgb(20000 / 65535.0, 20000 / 65535.0, bri / 65535.0 * 254.0)));
    const std::vector<RGB8> want = color_pipeline::render_strip(lamps, s, nullptr, nullptr, 0.0);
    assert(r.leds().size() == 20 && r.lamp_count() == 3 && r.frames() == 1 && r.phase() == "live" && r.driving());
    for (size_t i = 0; i < want.size(); ++i) assert(same(r.leds()[i], want[i]));
    assert(r.leds().front().r > r.leds().back().r);  // lamp 1 (bright) is at the start
    assert(r.current_ma() <= s.current_budget_ma + 1e-6);

    // RGB frames are levels, not sRGB
    huestream::Frame rgb;
    rgb.major = 1;
    rgb.colorspace = 0;
    rgb.channels = {{"1", 0x8000, 0, 0}, {"2", 0x8000, 0, 0}, {"3", 0x8000, 0, 0}};
    color_pipeline::StreamRenderer q;
    q.on_frame(rgb, s, 1.0);
    assert(q.leds()[10].r == 0x80 && q.leds()[10].g == 0);

    // frames flowing: tick does nothing
    assert(!r.tick(s, 10.05));
    // the stream stops: hold (same values), then fade, then black, then stopped
    assert(r.tick(s, 10.20) && r.phase() == "hold");
    for (size_t i = 0; i < want.size(); ++i) assert(same(r.leds()[i], want[i]));
    assert(!r.tick(s, 10.30));                       // hold repeats only every 0.5 s
    assert(r.tick(s, 10.75) && r.phase() == "hold");
    assert(r.tick(s, 11.50) && r.phase() == "fade");
    assert(r.leds().front().r < want.front().r && r.leds().front().r > 0);
    assert(r.tick(s, 11.54));                        // fading: every tick
    assert(r.tick(s, 12.10) && r.phase() == "black");
    for (const RGB8& c : r.leds()) assert(c.r == 0 && c.g == 0 && c.b == 0);
    assert(r.driving());
    assert(r.tick(s, 18.0) && r.phase() == "stopped" && !r.driving());
    assert(!r.tick(s, 19.0));
    // and comes back
    assert(r.on_frame(xy_frame(65535, 65535, 65535), s, 20.0) && r.phase() == "live" && r.driving());

    // rise limit: a jump from black is spread over rise_ms
    Settings slow = s;
    slow.rise_ms = 400;
    color_pipeline::StreamRenderer z;
    z.on_frame(xy_frame(0, 0, 0), slow, 0.0);
    z.on_frame(xy_frame(65535, 65535, 65535), slow, 0.04);
    const int first = z.leds()[10].r;
    z.on_frame(xy_frame(65535, 65535, 65535), slow, 0.50);
    assert(first > 0 && first < z.leds()[10].r);
    // v0.33: with the default rise_ms the very first frame, and the first frame
    // after a pause, no longer jump to the full budget in one step
    {
        Settings d;  // defaults: 43 LEDs, 450 mA, rise 120 ms
        d.fade_in_ms = 0;   // v0.42: the v0.33 soft start alone (fade_in / dark fade: test_soft_on_off)
        d.fade_out_ms = 0;
        d.smooth_ms = 0;    // v0.44
        color_pipeline::StreamRenderer w;
        w.on_frame(xy_frame(65535, 65535, 65535), d, 100.0);
        const double first_ma = w.current_ma();
        assert(first_ma < 0.5 * d.current_budget_ma);      // was 449.8 mA in one step
        double prev = first_ma;
        for (int i = 1; i <= 3; ++i) {
            w.on_frame(xy_frame(65535, 65535, 65535), d, 100.0 + 0.04 * i);
            assert(w.current_ma() >= prev && w.current_ma() <= d.current_budget_ma + 1e-6);
            prev = w.current_ma();
        }
        assert(prev > 0.95 * d.current_budget_ma);         // there after rise_ms
        w.on_frame(xy_frame(0, 0, 0), d, 100.16);
        w.on_frame(xy_frame(65535, 65535, 65535), d, 100.16 + 0.13);  // 130 ms pause, then white
        assert(w.current_ma() < 0.5 * d.current_budget_ma);
        w.on_frame(xy_frame(0, 0, 0), d, 100.33);
        w.on_frame(xy_frame(65535, 65535, 65535), d, 100.33 + 0.70);  // 700 ms: hold phase, then white
        assert(w.current_ma() < 0.5 * d.current_budget_ma);
    }
    // v0.33: the budget is lowered while the last frame is held / fading
    {
        Settings d;
        d.fade_in_ms = d.fade_out_ms = 0;  // v0.42: tested in test_soft_on_off
        d.smooth_ms = 0;  // v0.44: tested in test_smoothing
        d.rise_ms = 0;
        d.current_budget_ma = 800;
        color_pipeline::StreamRenderer w;
        w.on_frame(xy_frame(65535, 65535, 65535), d, 50.0);
        assert(w.current_ma() > 700.0);
        d.current_budget_ma = 300;
        assert(w.tick(d, 50.2) && w.phase() == "hold");
        assert(w.current_ma() <= 300.0 && w.current_ma() > 250.0);
        assert(w.tick(d, 51.3) && w.phase() == "fade" && w.current_ma() <= 300.0);
    }
    std::printf("[ok] StreamRenderer: same output as the manual steps, lamp order, RGB frames, hold/fade/black/stopped, rise\n");
}

int main() {
    test_stream_renderer();
    test_soft_on_off();
    test_smoothing();
    test_js_pair_signature();
    test_white_balance();
    test_tv_lock_rules();
    test_brightness();
    test_custom_lamp_positions();
    test_stream_to_strip();
    std::printf("ALL SHARED PIPELINE TESTS PASSED\n");
    return 0;
}
