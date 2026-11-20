// Plain assert-based tests for the pure logic (parser, colour pipeline,
// settings persistence). No network, no mbedTLS - these run anywhere,
// including this cloud sandbox, and should keep passing unmodified once
// ported into the ESP32/WLED build.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "huestream.hpp"
#include "color_pipeline.hpp"
#include "settings.hpp"
#include "identity_store.hpp"
#include "discovery.hpp"
#include "strip_stats.hpp"
#include <cstdlib>
#include <cjson/cJSON.h>
#include <unistd.h>
#include "pairing_store.hpp"
#include "bridge_identity.hpp"

using namespace color_pipeline;

static std::vector<uint8_t> build_v1_frame(uint8_t colorspace,
                                            const std::vector<std::tuple<uint16_t, uint16_t, uint16_t, uint16_t>>& channels) {
    std::vector<uint8_t> f;
    const char* magic = "HueStream";
    f.insert(f.end(), magic, magic + 9);
    f.push_back(1);              // major
    f.push_back(0);              // minor
    f.push_back(7);              // seq
    f.push_back(0);               // reserved
    f.push_back(0);               // reserved
    f.push_back(colorspace);      // colorspace
    f.push_back(0);               // reserved (pad to 16 bytes total)
    for (auto& [lamp_id, a, b, c] : channels) {
        f.push_back(0);  // type
        f.push_back(lamp_id >> 8);
        f.push_back(lamp_id & 0xFF);
        f.push_back(a >> 8); f.push_back(a & 0xFF);
        f.push_back(b >> 8); f.push_back(b & 0xFF);
        f.push_back(c >> 8); f.push_back(c & 0xFF);
    }
    return f;
}

// v0.30: the tests written for v0.13..v0.29 check the inset layout (now
// strip_layout "inset"); the default became the tv layout (test_v030_layout).
// v0.33: the budget has a build-time ceiling (kMaxBudgetMa), so "no limit" for a
// test of the fill/geometry is no longer a huge budget: make the LEDs so
// frugal that no frame can reach the ceiling (1 mA per LED, no idle draw).
static void no_limit(color_pipeline::Settings& s) {
    s.current_budget_ma = color_pipeline::kMaxBudgetMa;
    s.ma_per_led_full_white = 1.0;
    s.idle_ma_per_led = 0.0;
}

static color_pipeline::Settings legacy_layout() {
    color_pipeline::Settings s;
    s.strip_layout = color_pipeline::kLayoutInset;
    s.current_budget_ma = 800.0;  // the default until v0.31; these tests' numbers were made with it
    return s;
}

static void test_huestream_v1_xy() {
    auto f = build_v1_frame(1, {{1, 19660, 19660, 30840}, {2, 0, 0, 0}, {3, 65535, 65535, 65535}});
    huestream::Frame out;
    bool ok = huestream::parse_frame(f, out);
    assert(ok);
    assert(out.major == 1 && out.colorspace == 1);
    assert(out.channels.size() == 3);
    assert(out.channels[0].lamp_id == "1");
    assert(out.channels[0].a == 19660);
    assert(out.channels[2].c == 65535);
    printf("[ok] huestream v1 XY parse\n");
}

static void test_huestream_v2_rgb() {
    std::vector<uint8_t> f;
    const char* magic = "HueStream";
    f.insert(f.end(), magic, magic + 9);
    f.push_back(2); f.push_back(0); f.push_back(0);
    f.resize(16, 0);
    f[14] = 0;  // colorspace = RGB
    f.resize(52, 0);  // pad to v2 header size
    // one channel: id=0 -> lamp "1", RGB = (65535,0,32768)
    f.push_back(0);
    f.push_back(0xFF); f.push_back(0xFF);
    f.push_back(0x00); f.push_back(0x00);
    f.push_back(0x80); f.push_back(0x00);
    huestream::Frame out;
    bool ok = huestream::parse_frame(f, out);
    assert(ok);
    assert(out.major == 2);
    assert(out.channels.size() == 1);
    assert(out.channels[0].lamp_id == "1");
    assert(rgb16_to_8(out.channels[0].a) == 255);
    assert(rgb16_to_8(out.channels[0].b) == 0);
    printf("[ok] huestream v2 RGB parse\n");
}

static void test_huestream_rejects_garbage() {
    std::vector<uint8_t> junk = {1, 2, 3, 4};
    huestream::Frame out;
    assert(!huestream::parse_frame(junk, out));
    printf("[ok] huestream rejects short/bad-magic frames\n");
}

static void test_xy_bri_to_rgb_matches_python_reference() {
    // Values transcribed from fakebridge.py's own xy_bri_to_rgb() for a few
    // known inputs (full red point, full brightness white point, black).
    RGB8 black = xy_bri_to_rgb(0.3, 0.3, 0.0);
    assert(black.r == 0 && black.g == 0 && black.b == 0);

    RGB8 zero_y = xy_bri_to_rgb(0.3, 0.0, 200.0);
    assert(zero_y.r == 0 && zero_y.g == 0 && zero_y.b == 0);  // y<=0 guard

    // Hue "red" point (0.6915, 0.3083) at near-full brightness should come
    // out strongly red-dominant after normalisation.
    RGB8 red = xy_bri_to_rgb(0.6915, 0.3083, 254.0);
    assert(red.r > red.g && red.r > red.b);
    assert(red.r == 255);  // normalised so the max channel always hits 255
    printf("[ok] xy_bri_to_rgb (ported formula) sanity checks\n");
}

// v0.35: the two rules for OUR part of "black picture -> white strip" (the TV's own
// values are in docs/STATUS.md): bri 0 from the TV -> (0,0,0), whatever x / y; and
// y = 0 (or a y so small it is really 0) never turns into white by a choice of ours.
// Inputs are the raw 16-bit HueStream values, converted exactly like main.cpp does.
static RGB8 from_raw16(unsigned x16, unsigned y16, unsigned bri16) {
    return xy_bri_to_rgb(x16 / 65535.0, y16 / 65535.0, bri16 / 65535.0 * 254.0);
}
static void test_black_and_zero_y_rules() {
    const unsigned xs[] = {0, 1, 100, 20176, 32768, 45000, 65535};
    const unsigned ys[] = {0, 1, 2, 10, 100, 21194, 32768, 65535};
    const unsigned bris[] = {0, 1, 4112, 19789, 65535};
    int cases = 0;
    for (unsigned x : xs)
        for (unsigned y : ys) {
            RGB8 c = from_raw16(x, y, 0);
            assert(c.r == 0 && c.g == 0 && c.b == 0);  // bri 0 -> black, any x / y
            assert(srgb_to_led(c).r == 0 && srgb_to_led(c).g == 0 && srgb_to_led(c).b == 0);
            ++cases;
        }
    for (unsigned x : xs)
        for (unsigned b : bris) {
            RGB8 c = from_raw16(x, 0, b);
            assert(c.r == 0 && c.g == 0 && c.b == 0);  // y = 0 -> black (guard), never white
            ++cases;
            for (unsigned y : {1u, 2u, 10u}) {      // y next to 0: no white either
                RGB8 d = from_raw16(x, y, b);
                assert(!(d.r >= 200 && d.g >= 200 && d.b >= 200));
                ++cases;
            }
        }
    // x = y = 0 with brightness: black, not white
    RGB8 z = from_raw16(0, 0, 65535);
    assert(z.r == 0 && z.g == 0 && z.b == 0);
    // A black strip from black lamps: every LED 0 after the renderer too.
    Settings s;
    std::vector<RGB8> lamps(5, from_raw16(20176, 21194, 0));
    for (auto& l : lamps) l = srgb_to_led(l);
    auto leds = render_strip(lamps, s);
    for (auto& l : leds) assert(l.r == 0 && l.g == 0 && l.b == 0);
    // What the TV really sent on 2026-11-06 (results/patterns/20261106T205508Z), kept as
    // reference numbers: black picture = xy (0.3079, 0.3234) at bri16 4112 (16/255),
    // NOT bri 0, so the strip gets a dim grey from the TV's values, not from us.
    RGB8 tv_black = from_raw16(20176, 21194, 4112);
    assert(tv_black.r == 68 && tv_black.g == 72 && tv_black.b == 74);
    // D65 at full brightness is (near) white with this matrix ...
    RGB8 d65 = xy_bri_to_rgb(0.3127, 0.3290, 254.0);
    assert(d65.r >= 240 && d65.g >= 240 && d65.b >= 240);
    // ... while the TV's "white" xy (0.2658, 0.2718) is clearly blue: the blue comes
    // with the TV's numbers, the matrix only adds a few % (r of D65 = 245, not 255).
    RGB8 tv_white = xy_bri_to_rgb(17419 / 65535.0, 17813 / 65535.0, 19789 / 65535.0 * 254.0);
    assert(tv_white.b > tv_white.r + 40);
    printf("[ok] bri 0 -> black, y 0 -> never white (%d raw cases); TV black (68,72,74), D65 (%d,%d,%d), TV white (%d,%d,%d)\n",
           cases, d65.r, d65.g, d65.b, tv_white.r, tv_white.g, tv_white.b);
}

#include "color_trace.hpp"
static void test_color_trace_line() {
    color_trace::Frame f;
    f.seq = 7;
    f.colorspace = 1;
    color_trace::Lamp l;
    l.id = "2";
    l.raw[0] = 20176; l.raw[1] = 21194; l.raw[2] = 4112;
    l.rgb = {68, 72, 74};
    l.led = srgb_to_led(l.rgb);
    f.lamps.push_back(l);
    f.js_age_s = 0.25;
    f.js_left = {{1, 2, 3}};
    f.js_top = {};
    f.js_right = {{23, 0, 0}, {0, 23, 0}};
    f.ddp_payload = {0x0f, 0x11, 0xff};
    f.ddp_sent = true;
    f.estimated_ma = 208.04;
    const std::string j = color_trace::Trace::to_json(f, 1000.5);
    cJSON* root = cJSON_Parse(j.c_str());
    assert(root);
    assert(j.back() == '\n');
    assert(cJSON_GetObjectItem(root, "seq")->valueint == 7);
    cJSON* lamp = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "lamps"), 0);
    assert(std::string(cJSON_GetObjectItem(lamp, "id")->valuestring) == "2");
    assert(cJSON_GetArrayItem(cJSON_GetObjectItem(lamp, "raw"), 2)->valueint == 4112);
    assert(std::fabs(cJSON_GetObjectItem(lamp, "x")->valuedouble - 0.3079) < 1e-4);
    assert(cJSON_GetObjectItem(lamp, "bri16")->valueint == 4112);
    assert(cJSON_GetArrayItem(cJSON_GetObjectItem(lamp, "led"), 0)->valueint == l.led.r);
    cJSON* js = cJSON_GetObjectItem(root, "js");
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(js, "right")) == 2);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(js, "top")) == 0);
    assert(std::string(cJSON_GetObjectItem(root, "ddp")->valuestring) == "0f11ff");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(root, "ddp_sent")));
    cJSON_Delete(root);
    printf("[ok] colour trace line is valid JSON with raw / rgb / led / js / ddp\n");
}

static void test_dedup_tolerance() {
    LampDedupState st;
    assert(xy_bri_changed(st, 0.30, 0.30, 100.0));       // first value always "changed"
    assert(!xy_bri_changed(st, 0.305, 0.305, 105.0));    // inside tolerance
    assert(xy_bri_changed(st, 0.40, 0.30, 100.0));       // x jumped past tolerance
    printf("[ok] per-lamp tolerance dedup\n");
}

static void test_render_strip_fits_budget_under_pressure() {
    Settings s = legacy_layout();
    s.peak_emphasis = 0.0;  // this test pins the pre-v0.13 anchor behaviour
    s.led_count = 43;
    s.current_budget_ma = 465.0;
    s.idle_ma_per_led = 1.0;
    s.ma_per_led_full_white = 60.0;

    // Three lamps, all full white: the "ideal" 43-LED frame would need
    // ~43*60 + 43*1 = 2623 mA, far over budget -> anchors must shrink.
    std::vector<RGB8> lamps = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    int used = -1;
    auto leds = render_strip(lamps, s, &used);
    assert(static_cast<int>(leds.size()) == s.led_count);  // never fewer physical LEDs lit
    double ma = estimate_current_ma(leds, s);
    assert(ma <= s.current_budget_ma + 1e-6);
    assert(used < s.led_count);  // it did have to coarsen
    assert(used >= s.min_anchors);
    printf("[ok] render_strip coarsens anchors to stay within budget (used=%d, ma=%.1f)\n", used, ma);
}

static void test_render_strip_full_resolution_when_dim() {
    Settings s = legacy_layout();
    s.led_count = 43;
    s.current_budget_ma = 465.0;

    // Dim content: well within budget, so full 43-point resolution expected.
    std::vector<RGB8> lamps = {{10, 10, 10}, {12, 8, 14}, {9, 11, 10}};
    int used = -1;
    auto leds = render_strip(lamps, s, &used);
    assert(used == s.led_count);
    double ma = estimate_current_ma(leds, s);
    assert(ma <= s.current_budget_ma);
    printf("[ok] render_strip keeps full resolution for dim content (ma=%.1f)\n", ma);
}

static void test_srgb_to_led_linear() {
    assert(srgb8_to_linear8(0) == 0);
    assert(srgb8_to_linear8(255) == 255);
    assert(srgb8_to_linear8(128) == 55);   // mid grey on a screen = ~22 % light
    assert(srgb8_to_linear8(10) == 1);     // linear segment of the sRGB curve
    for (int i = 1; i < 256; ++i) assert(srgb8_to_linear8(uint8_t(i)) >= srgb8_to_linear8(uint8_t(i - 1)));
    for (int i = 1; i < 255; ++i) assert(srgb8_to_linear8(uint8_t(i)) <= i);  // never brighter than sRGB
    RGB8 c = srgb_to_led({255, 128, 0});
    assert(c.r == 255 && c.g == 55 && c.b == 0);
    // Full white stays full white, so the budget test above is unchanged;
    // mid grey now costs ~22 % of the current instead of ~50 %.
    Settings s = legacy_layout();
    std::vector<RGB8> grey(43, RGB8{128, 128, 128}), grey_led(43, srgb_to_led({128, 128, 128}));
    assert(estimate_current_ma(grey_led, s) < 0.5 * estimate_current_ma(grey, s));
    printf("[ok] sRGB -> LED linear: 0/128/255 -> 0/55/255, monotonic, mid grey < half the current\n");
}


// ---- Phase 2 (v0.13) -------------------------------------------------------
// Verbatim copy of render_strip() as it was before v0.13 (v0.12), kept here
// as the reference for "peak_emphasis = 0 behaves exactly like before".
namespace legacy {
struct RGBf { double r = 0, g = 0, b = 0; };
static RGBf to_f(const RGB8& c) { return {double(c.r), double(c.g), double(c.b)}; }
static RGB8 to_8(const RGBf& c) {
    auto clamp8 = [](double v) { return static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(v)))); };
    return {clamp8(c.r), clamp8(c.g), clamp8(c.b)};
}
static std::vector<RGBf> resample(const std::vector<RGBf>& anchors, int count) {
    std::vector<RGBf> out(std::max(count, 0));
    const int k = static_cast<int>(anchors.size());
    if (k == 0 || count <= 0) return out;
    if (k == 1) { std::fill(out.begin(), out.end(), anchors[0]); return out; }
    for (int i = 0; i < count; ++i) {
        double pos = (count == 1) ? 0.5 : double(i) / double(count - 1);
        double scaled = pos * (k - 1);
        int seg = static_cast<int>(std::floor(scaled));
        seg = std::max(0, std::min(k - 2, seg));
        double t = scaled - seg;
        const RGBf& a = anchors[seg];
        const RGBf& b = anchors[seg + 1];
        out[i] = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    }
    return out;
}
static std::vector<RGB8> render_at(const std::vector<RGB8>& lamps, int anchors, int led_count) {
    std::vector<RGBf> lamp_f;
    for (const auto& c : lamps) lamp_f.push_back(to_f(c));
    std::vector<RGBf> anchor_f = resample(lamp_f, std::max(anchors, 1));
    std::vector<RGBf> led_f = resample(anchor_f, led_count);
    std::vector<RGB8> out(led_f.size());
    for (size_t i = 0; i < led_f.size(); ++i) out[i] = to_8(led_f[i]);
    return out;
}
static std::vector<RGB8> render_strip(const std::vector<RGB8>& lamps, const Settings& settings, int* used_anchors_out) {
    const int led_count = std::max(settings.led_count, 1);
    if (lamps.empty()) { if (used_anchors_out) *used_anchors_out = 0; return std::vector<RGB8>(led_count, RGB8{0, 0, 0}); }
    int hi = led_count;
    int lo = std::max(settings.min_anchors, 1);
    lo = std::min(lo, hi);
    std::vector<RGB8> best = render_at(lamps, hi, led_count);
    if (estimate_current_ma(best, settings) <= settings.current_budget_ma || hi == lo) {
        if (used_anchors_out) *used_anchors_out = hi;
        return best;
    }
    int best_m = lo;
    std::vector<RGB8> best_frame = render_at(lamps, lo, led_count);
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        std::vector<RGB8> frame = render_at(lamps, mid, led_count);
        if (estimate_current_ma(frame, settings) <= settings.current_budget_ma) { best_m = mid; best_frame = frame; lo = mid + 1; }
        else hi = mid - 1;
    }
    double current = estimate_current_ma(best_frame, settings);
    if (current > settings.current_budget_ma) {
        double idle = settings.idle_ma_per_led * best_frame.size();
        double color_current = current - idle;
        double available_for_color = settings.current_budget_ma - idle;
        double scale = 1.0;
        if (color_current > 0.0) { scale = std::max(0.0, available_for_color / color_current); scale = std::min(scale, 1.0); }
        for (auto& c : best_frame) {
            c.r = static_cast<uint8_t>(std::floor(c.r * scale));
            c.g = static_cast<uint8_t>(std::floor(c.g * scale));
            c.b = static_cast<uint8_t>(std::floor(c.b * scale));
        }
    }
    if (used_anchors_out) *used_anchors_out = best_m;
    return best_frame;
}
}  // namespace legacy

static uint32_t g_rng = 12345;
static uint32_t rnd() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static std::vector<RGB8> random_lamps(int n) {
    std::vector<RGB8> v(n);
    int style = rnd() % 4;  // 0 random, 1 bright, 2 white-ish, 3 dark
    for (auto& c : v) {
        auto ch = [&]() -> uint8_t {
            switch (style) {
                case 1: return uint8_t(128 + rnd() % 128);
                case 2: return uint8_t(230 + rnd() % 26);
                case 3: return uint8_t(rnd() % 40);
                default: return uint8_t(rnd() % 256);
            }
        };
        c = {ch(), ch(), ch()};
    }
    return v;
}

static void test_budget_never_exceeded_fuzz() {
    const double pes[] = {0.0, 0.1, 0.5, 0.85, 1.0};
    const double budgets[] = {60.0, 120.0, 300.0, 465.0, 900.0, 3000.0};
    const int counts[] = {1, 2, 27, 43, 60};
    int cases = 0, limited = 0;
    for (int iter = 0; iter < 300; ++iter) {
        for (double pe : pes) {
            Settings s = legacy_layout();
            s.peak_emphasis = pe;
            s.led_count = counts[rnd() % 5];
            s.current_budget_ma = budgets[rnd() % 6];
            s.ma_per_led_full_white = (rnd() % 2) ? 60.0 : 35.0;
            s.idle_ma_per_led = (rnd() % 2) ? 1.0 : 0.5;
            s.peak_points = (rnd() % 3 == 0) ? int(1 + rnd() % 6) : 0;
            auto lamps = random_lamps(1 + rnd() % 5);
            RenderInfo info;
            auto out = render_strip(lamps, s, &info);
            assert(static_cast<int>(out.size()) == s.led_count);
            double idle = s.idle_ma_per_led * s.led_count;
            double ma = estimate_current_ma(out, s);
            // Hard limit. (If the budget is below even the idle draw, nothing
            // can fit: then every LED must be black.)
            bool ok = true;
            if (s.current_budget_ma >= idle) {
                ok = ma <= s.current_budget_ma + 1e-9;
            } else {
                for (auto& c : out) ok = ok && !(c.r || c.g || c.b);
            }
            if (!ok) {
                fprintf(stderr, "FAIL pe=%.2f leds=%d budget=%.0f idle=%.1f ma=%.1f mode=%s lamps=%zu\n", pe,
                        s.led_count, s.current_budget_ma, idle, ma, info.limit_mode, lamps.size());
                assert(false);
            }
            if (std::string(info.limit_mode) != "none") ++limited;
            ++cases;
        }
    }
    printf("[ok] budget never exceeded: %d random cases, %d of them limited (pe 0..1, 1-60 LEDs, 60-3000 mA)\n", cases,
           limited);
}

// ---- v0.23 (NEXT 9): «Απαλότητα ορίων» --------------------------------
static void test_blend_softness() {
    // softness 1 == smooth (within rounding), for zones and zones_mid, with
    // and without inset, held corners (corner_mode lamp).
    int compared = 0;
    for (int iter = 0; iter < 200; ++iter) {
        for (int mode : {color_pipeline::kBlendZones, color_pipeline::kBlendZonesMid}) {
            Settings s = legacy_layout();
            no_limit(s);  // compare the fill only
            s.led_count = 10 + int(rnd() % 50);
            s.lamp_inset = (rnd() % 2) ? 0.25 : 0.0;
            s.corner_mode = color_pipeline::kCornerLamp;
            auto lamps = random_lamps(2 + rnd() % 4);
            Settings smooth = s;
            smooth.blend_mode = color_pipeline::kBlendSmooth;
            Settings soft = s;
            soft.blend_mode = mode;
            soft.blend_softness = 1.0;
            auto a = render_strip(lamps, smooth, nullptr, nullptr);
            auto b = render_strip(lamps, soft, nullptr, nullptr);
            assert(a.size() == b.size());
            for (size_t i = 0; i < a.size(); ++i) {
                assert(std::abs(int(a[i].r) - int(b[i].r)) <= 1);
                assert(std::abs(int(a[i].g) - int(b[i].g)) <= 1);
                assert(std::abs(int(a[i].b) - int(b[i].b)) <= 1);
            }
            ++compared;
        }
    }
    // softness 0 keeps hard zones: every LED is exactly one of the lamp colours.
    {
        Settings s = legacy_layout();
        no_limit(s);
        s.blend_mode = color_pipeline::kBlendZones;
        s.blend_softness = 0.0;
        std::vector<RGB8> lamps = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        for (auto& c : render_strip(lamps, s, nullptr, nullptr)) {
            bool one = false;
            for (auto& l : lamps) one = one || (c.r == l.r && c.g == l.g && c.b == l.b);
            assert(one);
        }
    }
    // A soft border is a monotonic ramp, flat zones stay flat in their middle.
    {
        Settings s = legacy_layout();
        no_limit(s);
        s.led_count = 43;
        s.lamp_inset = 0.0;
        s.blend_mode = color_pipeline::kBlendZones;
        s.blend_softness = 0.3;
        std::vector<RGB8> lamps = {{0, 0, 0}, {200, 200, 200}};
        auto out = render_strip(lamps, s, nullptr, nullptr);
        for (size_t i = 1; i < out.size(); ++i) assert(out[i].r >= out[i - 1].r);
        assert(out.front().r == 0 && out.back().r == 200);
        int ramp = 0;
        for (auto& c : out) ramp += (c.r > 0 && c.r < 200) ? 1 : 0;
        // 30 % of the 42-LED span between the two lamps ~ 12 LEDs, never the whole strip
        assert(ramp >= 8 && ramp <= 16);
        Settings hard = s;
        hard.blend_softness = 0.0;
        int ramp0 = 0;
        for (auto& c : render_strip(lamps, hard, nullptr, nullptr)) ramp0 += (c.r > 0 && c.r < 200) ? 1 : 0;
        assert(ramp0 == 0);
    }
    printf("[ok] blend_softness: 1 == smooth in %d random cases, 0 = hard zones, 0.3 = monotonic ramp of ~30 %%\n", compared);
}

// ---- v0.24: end_lamps, auto/triangle drift, stream loss, corner fade ------
static void test_v024_pipeline() {
    Settings s = legacy_layout();
    s.led_count = 43;
    // v0.33: no limit for the position checks = frugal LEDs under the 900 mA ceiling
    s.current_budget_ma = color_pipeline::kMaxBudgetMa;
    s.ma_per_led_full_white = 15.0;
    s.lamp_inset = 0.25;
    s.end_lamps = true;
    auto p5 = strip_lamp_positions(s, 5);
    assert(p5.size() == 5 && p5[0] == 0.0 && p5[1] == 0.25 && p5[2] == 0.5 && p5[3] == 0.75 && p5[4] == 1.0);
    auto p3 = strip_lamp_positions(s, 3);
    assert(p3 == lamp_positions(3, 0.25));  // ignored with 3 lamps
    assert(end_lamps_active(s, 5) && !end_lamps_active(s, 3));
    Settings zero = s;
    zero.lamp_inset = 0.0;
    assert(!end_lamps_active(zero, 5));

    std::vector<RGB8> five = {{200, 0, 0}, {0, 200, 0}, {0, 0, 200}, {200, 200, 0}, {0, 200, 200}};
    Corners corners;
    corners.has_left = corners.has_right = true;
    corners.left = {255, 255, 255};
    corners.right = {255, 255, 255};
    for (int blend : {kBlendSmooth, kBlendZones, kBlendZonesMid}) {
        Settings x = s;
        x.blend_mode = blend;
        x.corner_mode = kCornerSide;
        auto out = render_strip(five, x, nullptr, &corners);
        auto out_nc = render_strip(five, x, nullptr, nullptr);
        assert(out.size() == 43);
        // ends = lamp 1 / lamp 5, the middle = lamp 3; JointSpace corners not used
        assert(out[0].r == 200 && out[0].g == 0 && out[0].b == 0);
        assert(out[42].r == 0 && out[42].g == 200 && out[42].b == 200);
        assert(out[21].r == 0 && out[21].g == 0 && out[21].b == 200);
        for (size_t i = 0; i < out.size(); ++i) assert(out[i].r == out_nc[i].r && out[i].g == out_nc[i].g && out[i].b == out_nc[i].b);
    }
    // smooth: halfway between lamp 1 (LED 0) and lamp 2 (LED 10.5) is a mix
    {
        auto out = render_strip(five, s, nullptr, nullptr);
        assert(out[5].r > 60 && out[5].r < 140 && out[5].g > 60 && out[5].g < 140);
    }
    // 3 lamps: end_lamps changes nothing
    {
        std::vector<RGB8> three = {{200, 10, 0}, {0, 200, 30}, {40, 0, 200}};
        Settings off = s;
        off.end_lamps = false;
        off.corner_mode = s.corner_mode = kCornerSide;
        for (double budget : {900.0, 150.0, 100.0}) {  // unlimited, limited, limited hard (15 mA per LED)
            off.current_budget_ma = s.current_budget_ma = budget;
            RenderInfo a, b;
            auto x = render_strip(three, s, &a, &corners, 77.0);
            auto y = render_strip(three, off, &b, &corners, 77.0);
            for (size_t i = 0; i < x.size(); ++i) assert(x[i].r == y[i].r && x[i].g == y[i].g && x[i].b == y[i].b);
            assert(a.peak_leds == b.peak_leds);
        }
        s.current_budget_ma = color_pipeline::kMaxBudgetMa;
    }
    // peaks only at lamps 2..4 (LED 10/11, 21, 31/32), never at the ends
    {
        Settings x = s;
        x.current_budget_ma = 800;
        x.ma_per_led_full_white = 60.0;  // back to real LEDs: white wants ~2.6 A, far over 800
        x.peak_emphasis = 0.8;
        x.peak_drift_leds = 0.0;
        std::vector<RGB8> white(5, RGB8{255, 255, 255});
        RenderInfo info;
        auto out = render_strip(white, x, &info, nullptr);
        assert(std::string(info.limit_mode) == "peaks");
        assert(info.peak_leds.size() == 3);
        assert(std::abs(info.peak_leds[0] - 10) <= 1 && info.peak_leds[1] == 21 && std::abs(info.peak_leds[2] - 32) <= 1);
        assert(out[0].r < out[21].r && out[42].r < out[21].r);
        assert(estimate_current_ma(out, x) <= x.current_budget_ma);
    }
    // the budget is hard in every combination
    int checked = 0;
    for (int it = 0; it < 3000; ++it) {
        Settings x = legacy_layout();
        x.end_lamps = rnd() % 2;
        x.led_count = 1 + int(rnd() % 60);
        x.current_budget_ma = 60 + double(rnd() % 1500);
        x.peak_emphasis = double(rnd() % 5) / 4.0;
        x.blend_mode = int(rnd() % 3);
        x.corner_mode = int(rnd() % 2);
        x.blend_softness = double(rnd() % 4) / 3.0;
        x.lamp_inset = double(rnd() % 4) * 0.1;
        x.peak_drift_leds = (rnd() % 2) ? -1.0 : double(rnd() % 8);
        x.peak_drift_shape = int(rnd() % 2);
        std::vector<RGB8> lamps;
        int k = (rnd() % 2) ? 5 : 3;
        for (int j = 0; j < k; ++j) lamps.push_back({uint8_t(rnd()), uint8_t(rnd()), uint8_t(rnd())});
        if (rnd() % 3 == 0) lamps.assign(k, RGB8{255, 255, 255});
        Corners c = corners;
        c.weight = double(rnd() % 5) / 4.0;
        auto out = render_strip(lamps, x, nullptr, &c, double(rnd() % 100000) / 7.0);
        assert(int(out.size()) == x.led_count);
        assert(estimate_current_ma(out, x) <= x.current_budget_ma + 1e-9 || x.current_budget_ma < x.led_count * x.idle_ma_per_led);
        ++checked;
    }

    // drift: auto = peak spacing (10.5 LEDs), triangle at constant speed
    {
        Settings d = legacy_layout();
        assert(d.peak_drift_leds < 0 && d.peak_drift_period_s == 300.0 && d.peak_drift_shape == kDriftTriangle);
        assert(std::fabs(peak_drift_amplitude(d, 3) - 10.5) < 1e-9);
        assert(std::fabs(peak_drift_amplitude(d, 5) - 5.25) < 1e-9);  // 5 lamps at 25..75 %: spacing 12.5 %
        Settings e = d;
        e.end_lamps = true;
        assert(std::fabs(peak_drift_amplitude(e, 5) - 10.5) < 1e-9);  // end_lamps: peaks at lamps 2..4
        assert(peak_drift_offset(d, 0.0) == 0.0);
        assert(std::fabs(peak_drift_offset(d, 37.5) - 5.25) < 1e-9);   // 1/8 cycle: half way, linear
        assert(std::fabs(peak_drift_offset(d, 75.0) - 10.5) < 1e-9);
        assert(std::fabs(peak_drift_offset(d, 150.0)) < 1e-9);
        assert(std::fabs(peak_drift_offset(d, 225.0) + 10.5) < 1e-9);
        assert(std::fabs(peak_drift_offset(d, 375.0) - 10.5) < 1e-9);  // next cycle
        Settings sn = d;
        sn.peak_drift_shape = kDriftSine;
        assert(std::fabs(peak_drift_offset(sn, 37.5) - 10.5 * std::sin(3.14159265358979323846 / 4)) < 1e-9);
        // with auto the peaks reach every LED over a cycle
        std::vector<int> hit(43, 0);
        Settings w = d;
        w.peak_emphasis = 0.8;
        w.current_budget_ma = 800;
        std::vector<RGB8> white(3, RGB8{255, 255, 255});
        for (int step = 1; step <= 600; ++step) {
            RenderInfo info;
            render_strip(white, w, &info, nullptr, step * 0.5);
            for (int p : info.peak_leds) hit[p] = 1;
        }
        int covered = 0;
        for (int h : hit) covered += h;
        assert(covered == 43);
    }

    // stream loss: hold 1 s, (1-x)^2 fade over 1 s, black 30 s, stop
    {
        Settings l = legacy_layout();
        auto a = stream_loss_output(l, 500);
        assert(a.send && a.factor == 1.0);
        auto b = stream_loss_output(l, 1500);
        assert(b.send && std::fabs(b.factor - 0.25) < 1e-12);
        auto c = stream_loss_output(l, 2500);
        assert(c.send && c.factor == 0.0);
        auto d = stream_loss_output(l, 31999);
        assert(d.send && d.factor == 0.0);
        auto e = stream_loss_output(l, 32001);
        assert(!e.send);
        Settings nf = l;
        nf.loss_fade_ms = 0;
        assert(stream_loss_output(nf, 1001).factor == 0.0);
        std::vector<RGB8> f = {{255, 101, 3}};
        auto h = scale_frame(f, 0.5);
        assert(h[0].r == 127 && h[0].g == 50 && h[0].b == 1);
        assert(scale_frame(f, 0.0)[0].r == 0 && scale_frame(f, 1.0)[0].g == 101);
    }

    // corner weight: 0 = the lamp, 1 = the v0.18 half/half mix, between = between
    {
        Settings x = legacy_layout();
        x.corner_mode = kCornerSide;
        no_limit(x);
        std::vector<RGB8> three = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        Corners c;
        c.has_left = true;
        c.left = {200, 200, 200};
        c.weight = 1.0;
        assert(render_strip(three, x, nullptr, &c)[0].r == 100);
        c.weight = 0.5;
        assert(render_strip(three, x, nullptr, &c)[0].r == 50);
        c.weight = 0.0;
        assert(render_strip(three, x, nullptr, &c)[0].r == 0);
    }
    printf("[ok] v0.24: end_lamps positions/ends/peaks, %d budget cases, auto+triangle drift covers 43/43, loss fade, corner weight\n", checked);
}

// ---- v0.23 (NEXT 5): rolling peaks --------------------------------------
static void test_rolling_peaks() {
    Settings s = legacy_layout();
    s.led_count = 43;
    s.peak_emphasis = 0.8;
    s.current_budget_ma = 800;
    s.peak_drift_leds = 3.0;
    s.peak_drift_period_s = 60.0;
    std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    // time 0 (and drift 0) = the fixed v0.13 peaks, byte-identical
    Settings fixed = s;
    fixed.peak_drift_leds = 0.0;
    RenderInfo i0, i1;
    auto a = render_strip(white, s, &i0, nullptr, 0.0);
    auto b = render_strip(white, fixed, &i1, nullptr, 12345.6);
    assert(a.size() == b.size());
    for (size_t k = 0; k < a.size(); ++k) assert(a[k].r == b[k].r && a[k].g == b[k].g && a[k].b == b[k].b);
    assert(i0.peak_leds == i1.peak_leds);
    // a quarter period later the peaks have moved by +3 LEDs, half a period later back
    RenderInfo iq, ih, i3;
    render_strip(white, s, &iq, nullptr, 15.0);
    render_strip(white, s, &ih, nullptr, 30.0);
    render_strip(white, s, &i3, nullptr, 45.0);
    assert(std::fabs(iq.peak_offset_leds - 3.0) < 1e-9);
    assert(std::fabs(ih.peak_offset_leds) < 1e-9);
    assert(std::fabs(i3.peak_offset_leds + 3.0) < 1e-9);
    assert(iq.peak_leds.size() == i0.peak_leds.size());
    // each peak LED index moves by 3 (+-1: half-way positions like 10.5 round either way)
    double mean_q = 0, mean_3 = 0;
    for (size_t k = 0; k < iq.peak_leds.size(); ++k) {
        assert(std::abs(iq.peak_leds[k] - (i0.peak_leds[k] + 3)) <= 1);
        assert(std::abs(i3.peak_leds[k] - (i0.peak_leds[k] - 3)) <= 1);
        mean_q += iq.peak_leds[k] - i0.peak_leds[k];
        mean_3 += i3.peak_leds[k] - i0.peak_leds[k];
    }
    mean_q /= iq.peak_leds.size();
    mean_3 /= i3.peak_leds.size();
    assert(std::fabs(mean_q - 3.0) <= 0.7 && std::fabs(mean_3 + 3.0) <= 0.7);
    // the budget stays hard at every moment; pe = 0 ignores the clock
    for (int step = 0; step < 240; ++step) {
        double t = step * 0.25;
        for (double pe : {0.0, 0.5, 1.0}) {
            Settings x = s;
            x.peak_emphasis = pe;
            x.led_count = 1 + int(rnd() % 60);
            x.current_budget_ma = 100 + double(rnd() % 1500);
            x.blend_mode = int(rnd() % 3);
            x.blend_softness = double(rnd() % 4) / 3.0;
            x.lamp_inset = (rnd() % 2) ? 0.25 : 0.0;
            auto lamps = random_lamps(1 + rnd() % 5);
            auto out = render_strip(lamps, x, nullptr, nullptr, t);
            double idle = x.idle_ma_per_led * x.led_count;
            if (x.current_budget_ma >= idle) assert(estimate_current_ma(out, x) <= x.current_budget_ma + 1e-9);
            if (pe == 0.0) {
                auto ref = render_strip(lamps, x, nullptr, nullptr, 0.0);
                for (size_t k = 0; k < out.size(); ++k) assert(out[k].r == ref[k].r && out[k].g == ref[k].g && out[k].b == ref[k].b);
            }
        }
    }
    // wanted_ma: what the picture asked for before the limit
    assert(i0.wanted_ma > s.current_budget_ma && estimate_current_ma(a, s) <= s.current_budget_ma);
    printf("[ok] rolling peaks: t=0 == fixed peaks, +-3 LEDs at 1/4 and 3/4 period, budget hard over 720 timed cases (all blend modes x softness 0..1), pe=0 ignores time\n");
}

// ---- v0.23 (NEXT 6): per-LED log + limiter compromise episodes ----------
// v0.41: since_install (queue item 5), no wall clock (ESP32 without NTP), Storage, the text JSON
static void test_strip_stats_v041() {
    Settings s = legacy_layout();
    s.led_count = 10;
    s.ma_per_led_full_white = 40;
    s.current_budget_ma = 200;
    std::string saved;
    int saves = 0;
    strip_stats::Storage st_io;
    st_io.load = [&] { return saved; };
    st_io.save = [&](const std::string& t) { saved = t; ++saves; return true; };
    std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    {
        strip_stats::Stats st(st_io);
        // no clock: totals count, days do not, since stays "" until the clock is known
        for (int i = 0; i < 100; ++i) {
            RenderInfo info;
            auto out = render_strip(white, s, &info, nullptr, i * 0.04);
            st.add_frame(out, s, info, 1000.0 + i * 0.04, 0.04, false);
        }
        assert(std::fabs(st.all_time().stream_s - 4.0) < 1e-6);
        assert(st.days().empty() && st.all_time().since.empty());
        assert(st.episodes().size() == 1 && st.episodes()[0].start.empty());
        // not marked as mounted: install totals stay empty, the wear list is empty
        assert(st.install_mah().empty() && st.since_install().frames == 0);
        // marked (still no clock): counted from now on
        st.reset_install(1004.0);
        assert(!st.since_install().since.empty());
        const double t0 = 1793500000.0;
        for (int i = 0; i < 50; ++i) {
            RenderInfo info;
            auto out = render_strip(white, s, &info, nullptr, i * 0.04);
            st.add_frame(out, s, info, t0 + i * 0.04, 0.04, false);
        }
        assert(std::fabs(st.since_install().stream_s - 2.0) < 1e-6);
        assert(std::fabs(st.all_time().stream_s - 6.0) < 1e-6);
        assert(st.days().size() == 1 && !st.all_time().since.empty());
        assert(st.install_mah().size() == 10);
        // the page's JSON parses and carries the new keys
        cJSON* j = st.to_json(t0 + 2);
        assert(cJSON_IsTrue(cJSON_GetObjectItem(j, "installed")) && cJSON_IsTrue(cJSON_GetObjectItem(j, "clock_ok")));
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "since_install"), "mah")) == 10);
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(j, "wear")) == 10);
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(j, "episodes")) == 2);
        cJSON_Delete(j);
        // save_if_due: not before kSaveEveryS
        const int before = saves;  // reset_install() saved once already
        assert(!st.save_if_due(t0 + 2));  // first call only arms it
        assert(!st.save_if_due(t0 + 3));
        assert(st.save_if_due(t0 + 3 + strip_stats::kSaveEveryS) && saves == before + 1);
        assert(!st.save_if_due(t0 + 4 + strip_stats::kSaveEveryS));  // nothing new since
    }
    {
        strip_stats::Stats st(st_io);  // reload through Storage
        assert(std::fabs(st.since_install().stream_s - 2.0) < 0.06 && st.install_mah().size() == 10);
        assert(std::fabs(st.all_time().stream_s - 6.0) < 0.06 && st.days().size() == 1 && st.episodes().size() == 2);
    }
    printf("[ok] strip_stats v0.41: no clock (totals yes, days no), since_install only after marking, wear from it, Storage save/reload, save_if_due\n");
}

static void test_strip_stats() {
    const std::string path = "/tmp/ambi_test_strip_stats.json";
    std::remove(path.c_str());
    Settings s = legacy_layout();
    s.led_count = 10;
    s.ma_per_led_full_white = 60;
    s.idle_ma_per_led = 1;
    s.current_budget_ma = 200;
    s.peak_emphasis = 0.8;
    const double t0 = 1793500000.0;  // fixed wall clock
    {
        strip_stats::Stats st(path);
        std::vector<RGB8> dim(10, RGB8{25, 25, 25});
        std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
        double t = t0;
        // 10 s of dim, unlimited frames at 25 fps
        for (int i = 0; i < 250; ++i) {
            RenderInfo info;
            auto out = render_strip({{25, 25, 25}}, s, &info);
            st.add_frame(out, s, info, t, 0.04, false);
            t += 0.04;
        }
        // two over-budget stretches, 2 s each, 5 s apart (-> 2 episodes); then one with a 1 s gap (-> merged)
        for (int ep = 0; ep < 2; ++ep) {
            for (int i = 0; i < 50; ++i) {
                RenderInfo info;
                auto out = render_strip(white, s, &info, nullptr, t - t0);
                st.add_frame(out, s, info, t, 0.04, false);
                t += 0.04;
            }
            for (int i = 0; i < 125; ++i) {
                RenderInfo info;
                auto out = render_strip({{25, 25, 25}}, s, &info);
                st.add_frame(out, s, info, t, 0.04, false);
                t += 0.04;
            }
        }
        for (int part = 0; part < 2; ++part) {
            for (int i = 0; i < 25; ++i) {
                RenderInfo info;
                auto out = render_strip(white, s, &info, nullptr, t - t0);
                st.add_frame(out, s, info, t, 0.04, false);
                t += 0.04;
            }
            for (int i = 0; i < 25 && part == 0; ++i) {  // 1 s calm in between: same episode
                RenderInfo info;
                auto out = render_strip({{25, 25, 25}}, s, &info);
                st.add_frame(out, s, info, t, 0.04, false);
                t += 0.04;
            }
        }
        // a stream pause (dt 30 s) is not streaming time
        {
            RenderInfo info;
            auto out = render_strip({{25, 25, 25}}, s, &info);
            st.add_frame(out, s, info, t + 30, 30.0, false);
        }
        auto all = st.all_time();
        assert(all.leds.size() == 10);
        double expect_s = (250 + 2 * 175 + 75) * 0.04;
        assert(std::fabs(all.stream_s - expect_s) < 1e-6);
        assert(std::fabs(all.over_s - 6.0) < 1e-6);  // 2 + 2 + 2 s over budget
        assert(all.frames_limited == 150);
        assert(all.max_wanted_ma > s.current_budget_ma);
        auto eps = st.episodes();
        assert(eps.size() == 3);  // newest first; the last two stretches merged
        for (auto& e : eps) {
            assert(e.wanted_ma > s.current_budget_ma);
            assert(e.sent_ma <= s.current_budget_ma + 1e-9);
            assert(e.mode == "peaks" && !e.peak_leds.empty());
            assert(e.min_scale < e.peak_scale);
        }
        assert(std::fabs(eps[0].duration_s - 2.0) < 1e-6 && std::fabs(eps[2].duration_s - 2.0) < 1e-6);
        // the peak LEDs got peak time, the others did not (a peak LED is never dimmed below the rest)
        double peak_s = 0, other_s = 0;
        for (size_t i = 0; i < all.leds.size(); ++i) {
            bool is_peak = std::find(eps[0].peak_leds.begin(), eps[0].peak_leds.end(), int(i)) != eps[0].peak_leds.end();
            (is_peak ? peak_s : other_s) += all.leds[i].s_peak;
        }
        assert(peak_s > 0);
        // mAh: 10 LEDs at ~1 + 0.098*60 mA for 10 s ~ 0.019 mAh per LED from the calm part alone
        assert(all.leds[0].mah > 0.015 && all.leds[0].mah < 1.0);
        auto days = st.days();
        assert(days.size() == 1 && days[0].episodes == 3);
        // reset clears only since_reset
        st.reset_since(t + 31);
        assert(st.since_reset().stream_s == 0 && st.since_reset().frames == 0);
        assert(st.all_time().stream_s == all.stream_s);
        assert(st.save());
    }
    // reload from disk: totals, days and episodes survive, episodes come back closed
    {
        strip_stats::Stats st(path);
        auto all = st.all_time();
        assert(all.leds.size() == 10 && std::fabs(all.stream_s - (250 + 2 * 175 + 75) * 0.04) < 0.06);
        assert(st.episodes().size() == 3 && !st.episodes()[0].open);
        assert(st.days().size() == 1);
        assert(st.since_reset().frames == 0);
    }
    // a corrupt file never crashes, it starts empty
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        std::fputs("{not json", f);
        std::fclose(f);
        strip_stats::Stats st(path);
        assert(st.all_time().frames == 0 && st.episodes().empty());
    }
    std::remove(path.c_str());
    printf("[ok] strip_stats: stream/over time, 3 episodes (gap 5 s split, 1 s merged), peaks counted, pause not counted, reset only since_reset, reload, corrupt file\n");
}

static void test_pe0_identical_to_before() {
    int cases = 0, fixed = 0;
    for (int iter = 0; iter < 2000; ++iter) {
        Settings s = legacy_layout();
        s.peak_emphasis = 0.0;
        s.lamp_inset = 0.0;  // v0.16: 0 = lamps at the ends, as in v0.12
        s.led_count = 1 + rnd() % 60;
        // v0.33: budgets up to the build-time ceiling (was 50..2049): above it the
        // limiter now works to the ceiling on purpose, so there is nothing to compare.
        s.current_budget_ma = 50.0 + rnd() % 851;
        s.ma_per_led_full_white = 20.0 + rnd() % 60;
        s.min_anchors = 1 + rnd() % 3;
        auto lamps = random_lamps(rnd() % 6);  // includes 0 lamps
        int used_new = -1, used_old = -2;
        auto a = render_strip(lamps, s, &used_new);
        auto b = legacy::render_strip(lamps, s, &used_old);
        assert(a.size() == b.size());
        bool same = used_new == used_old;
        for (size_t i = 0; i < a.size(); ++i) same = same && a[i].r == b[i].r && a[i].g == b[i].g && a[i].b == b[i].b;
        double idle = s.idle_ma_per_led * s.led_count;
        bool old_over = s.current_budget_ma >= idle && estimate_current_ma(b, s) > s.current_budget_ma;
        if (old_over) {
            // v0.12 bug, fixed in v0.13: with led_count <= min_anchors the
            // old code returned without checking the budget at all.
            assert(s.led_count <= s.min_anchors);
            assert(estimate_current_ma(a, s) <= s.current_budget_ma);
            ++fixed;
        } else {
            assert(same);
            ++cases;
        }
    }
    printf("[ok] peak_emphasis=0 byte-identical to the v0.12 render_strip in %d random cases; "
           "differs only in %d cases where v0.12 broke the budget (led_count <= min_anchors)\n", cases, fixed);
}

static void test_small_strip_budget_regression() {
    // v0.12 bug: led_count <= min_anchors skipped the budget check entirely.
    Settings s = legacy_layout();
    s.peak_emphasis = 0.0;
    s.led_count = 2;
    s.min_anchors = 2;
    s.current_budget_ma = 60.0;
    std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}};
    auto old = legacy::render_strip(white, s, nullptr);
    assert(estimate_current_ma(old, s) > s.current_budget_ma);  // the bug, reproduced
    auto out = render_strip(white, s, static_cast<int*>(nullptr));
    assert(estimate_current_ma(out, s) <= s.current_budget_ma);
    printf("[ok] 2-LED strip: v0.12 drew %.0f mA over a 60 mA budget, now %.1f mA\n", estimate_current_ma(old, s),
           estimate_current_ma(out, s));
}

static void test_uniform_white() {
    std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    Settings s = legacy_layout();  // 43 LEDs, 800 mA, 60 mA/LED: full white would need 2623 mA
    s.peak_emphasis = 0.0;
    auto flat = render_strip(white, s, static_cast<int*>(nullptr));
    for (auto& c : flat) assert(c.r == flat[0].r && c.g == flat[0].r && c.b == flat[0].r);  // even, still white
    assert(estimate_current_ma(flat, s) <= s.current_budget_ma);

    s.peak_emphasis = 1.0;
    RenderInfo info;
    auto pk = render_strip(white, s, &info);
    assert(std::string(info.limit_mode) == "peaks");
    assert(estimate_current_ma(pk, s) <= s.current_budget_ma);
    assert(info.peak_leds.size() == 3 && info.peak_leds[0] == 11 && info.peak_leds[1] == 21 && info.peak_leds[2] == 32);
    for (auto& c : pk) assert(c.r == c.g && c.g == c.b);  // still white, just dimmer
    // peaks brighter than the old uniform level, midpoints dimmer
    // v0.16 quarters: peaks at 25/50/75 % (LEDs ~10.5, 21, 31.5); midpoints
    // between them (~16, ~26) and the corners (0, 42) are the dim parts.
    assert(pk[21].r > flat[21].r && pk[11].r > flat[11].r);
    assert(pk[16].r < flat[16].r && pk[26].r < flat[26].r && pk[0].r < flat[0].r);
    // symmetric and smooth: brightness falls monotonically from a peak to the midpoint
    for (int i = 0; i < 43; ++i) assert(std::abs(int(pk[i].r) - int(pk[42 - i].r)) <= 1);
    for (int i = 22; i <= 26; ++i) assert(pk[i].r <= pk[i - 1].r);
    printf("[ok] uniform white: pe=0 even (%d), pe=1 peaks %d/%d/%d, midpoints %d, corners %d, both within %.0f mA\n",
           flat[0].r, pk[11].r, pk[21].r, pk[32].r, pk[16].r, pk[0].r, s.current_budget_ma);
}

static void test_peaks_keep_full_when_possible() {
    // A budget that fits if only the in-between LEDs dim: the peak LEDs must
    // come out exactly as rendered (100 %).
    std::vector<RGB8> lamps = {{200, 120, 40}, {40, 200, 120}, {120, 40, 200}};
    Settings s = legacy_layout();
    s.peak_emphasis = 1.0;
    s.peak_points = 0;
    s.ma_per_led_full_white = 30.0;  // v0.33: the whole frame must fit under the 900 mA ceiling
    s.current_budget_ma = color_pipeline::kMaxBudgetMa;
    auto full = render_strip(lamps, s, static_cast<int*>(nullptr));
    double need = estimate_current_ma(full, s);
    s.current_budget_ma = need * 0.8;
    RenderInfo info;
    auto out = render_strip(lamps, s, &info);
    assert(estimate_current_ma(out, s) <= s.current_budget_ma);
    for (int p : info.peak_leds) assert(out[p].r == full[p].r && out[p].g == full[p].g && out[p].b == full[p].b);
    assert(info.peak_scale == 1.0 && info.min_scale < 1.0);
    // and content that already fits is untouched for any pe
    s.current_budget_ma = need + 1.0;
    auto same = render_strip(lamps, s, &info);
    assert(std::string(info.limit_mode) == "none");
    for (size_t i = 0; i < same.size(); ++i) assert(same[i].r == full[i].r && same[i].g == full[i].g);
    // N uniform peaks
    s.peak_points = 4;
    s.current_budget_ma = need * 0.8;
    out = render_strip(lamps, s, &info);
    assert(info.peak_leds.size() == 4 && estimate_current_ma(out, s) <= s.current_budget_ma);
    printf("[ok] at 80%% of the needed current the peak LEDs stay at 100%% (min LED scale %.2f); fitting content untouched\n",
           info.min_scale);
}

// ---- v0.33: one current model, with a scale per colour channel ------------
static void test_channel_current_model() {
    std::mt19937 rng(33);
    auto rnd = [&]() { return static_cast<uint8_t>(rng() % 256); };
    // scales 1/1/1 (default) = the v0.32 formula, bit for bit
    for (int it = 0; it < 2000; ++it) {
        Settings s;
        s.idle_ma_per_led = (rng() % 300) / 100.0;
        s.ma_per_led_full_white = 20.0 + (rng() % 6000) / 100.0;
        std::vector<RGB8> f(1 + rng() % 60);
        for (auto& c : f) c = {rnd(), rnd(), rnd()};
        double old_total = s.idle_ma_per_led * f.size();
        for (const auto& c : f) {
            double frac = (double(c.r) + double(c.g) + double(c.b)) / (3.0 * 255.0);
            old_total += frac * s.ma_per_led_full_white;
        }
        assert(estimate_current_ma(f, s) == old_total);  // == on purpose
        const double level = (double(f[0].r) + double(f[0].g) + double(f[0].b)) / (3.0 * 255.0);
        assert(color_pipeline::led_current_ma(f[0], s) == s.idle_ma_per_led + level * s.ma_per_led_full_white);
    }
    // a scale moves only its own channel, linearly
    Settings s;
    s.idle_ma_per_led = 0;
    s.ma_per_led_full_white = 60;
    s.ma_scale_r = 1.5;
    s.ma_scale_g = 0.5;
    assert(std::fabs(estimate_current_ma({{255, 0, 0}}, s) - 30.0) < 1e-9);
    assert(std::fabs(estimate_current_ma({{0, 255, 0}}, s) - 10.0) < 1e-9);
    assert(std::fabs(estimate_current_ma({{0, 0, 255}}, s) - 20.0) < 1e-9);
    assert(std::fabs(estimate_current_ma({{255, 255, 255}}, s) - 60.0) < 1e-9);
    // the limiter uses the same model: a red frame is cut harder with a higher red scale
    for (double pe : {0.0, 0.5}) {
        Settings a;
        a.led_count = 43;
        a.current_budget_ma = 300;
        a.peak_emphasis = pe;
        Settings b = a;
        b.ma_scale_r = 2.0;
        const std::vector<RGB8> red = {{255, 0, 0}, {255, 0, 0}, {255, 0, 0}};
        auto fa = render_strip(red, a, nullptr, nullptr);
        auto fb = render_strip(red, b, nullptr, nullptr);
        assert(estimate_current_ma(fa, a) <= 300.0 && estimate_current_ma(fb, b) <= 300.0);
        long sa = 0, sb = 0;
        for (auto& c : fa) sa += c.r;
        for (auto& c : fb) sb += c.r;
        assert(sb < sa);
        // fuzz: the budget holds with any scales
        for (int it = 0; it < 300; ++it) {
            Settings x = a;
            x.ma_scale_r = 0.1 + (rng() % 290) / 100.0;
            x.ma_scale_g = 0.1 + (rng() % 290) / 100.0;
            x.ma_scale_b = 0.1 + (rng() % 290) / 100.0;
            x.current_budget_ma = 60 + rng() % 800;
            std::vector<RGB8> lamps = {{rnd(), rnd(), rnd()}, {rnd(), rnd(), rnd()}, {rnd(), rnd(), rnd()}};
            auto f = render_strip(lamps, x, nullptr, nullptr, it * 1.7);
            assert(estimate_current_ma(f, x) <= x.current_budget_ma + 1e-9);
        }
    }
    // limits + JSON round trip
    Settings q;
    q.ma_scale_r = 0.0;
    q.ma_scale_g = 99.0;
    q.ma_scale_b = std::nan("");
    settings::sanitize(q);
    assert(q.ma_scale_r == 0.1 && q.ma_scale_g == 3.0 && q.ma_scale_b == 1.0);
    Settings r;
    r.ma_scale_r = 1.25;
    r.ma_scale_b = 0.75;
    cJSON* rj = settings::to_json(r);
    Settings back;
    settings::merge_json(back, rj);
    cJSON_Delete(rj);
    assert(back.ma_scale_r == 1.25 && back.ma_scale_g == 1.0 && back.ma_scale_b == 0.75);
    printf("[ok] current model: scales 1/1/1 bit-identical to v0.32, per-channel scales, budget holds, limits\n");
}

// ---- v0.33: the first frame and the frame after a pause ramp too -------------
static void test_rise_from_shown() {
    using color_pipeline::rise_from_shown;
    using color_pipeline::RiseState;
    std::mt19937 rng(7);
    auto rnd8 = [&]() { return static_cast<uint8_t>(rng() % 256); };
    // frames flowing (no soft start pending, dt <= 100 ms, same size): exactly limit_rise
    for (int it = 0; it < 3000; ++it) {
        const size_t n = 1 + rng() % 50;
        std::vector<RGB8> p(n), t(n);
        for (auto& c : p) c = {rnd8(), rnd8(), rnd8()};
        for (auto& c : t) c = {rnd8(), rnd8(), rnd8()};
        const double dt = (rng() % 1001) / 10.0, rise = (rng() % 5 == 0) ? 0.0 : 1.0 + rng() % 600;
        RiseState st;
        auto a = rise_from_shown(st, p, t, dt, rise), b = limit_rise(p, t, dt, rise);
        for (size_t i = 0; i < n; ++i) assert(a[i].r == b[i].r && a[i].g == b[i].g && a[i].b == b[i].b);
        assert(st.remaining_ms == 0.0);
    }
    const std::vector<RGB8> white(5, RGB8{255, 255, 255}), black(5, RGB8{0, 0, 0});
    // first frame ever: a third of the way (40 of 120 ms), then 2/3, then there
    {
        RiseState st;
        auto f = rise_from_shown(st, {}, white, 12345.0, 120);
        assert(f.size() == 5 && f[0].r == 85 && f[4].b == 85 && st.remaining_ms == 80.0);
        f = rise_from_shown(st, f, white, 40.0, 120);
        assert(f[0].r == 170 && st.remaining_ms == 40.0);
        f = rise_from_shown(st, f, white, 40.0, 120);
        assert(f[0].r == 255 && st.remaining_ms == 0.0);
    }
    // the point of it: a LOW target (what the limiter leaves of full white) ramps too -
    // limit_rise alone reaches 40 in one step
    {
        const std::vector<RGB8> dim(5, RGB8{40, 40, 40});
        assert(limit_rise(black, dim, 40.0, 120)[0].r == 40);
        RiseState st;
        auto f = rise_from_shown(st, {}, dim, 0.0, 120);
        assert(f[0].r == 14);  // ceil(40 / 3)
        f = rise_from_shown(st, f, dim, 40.0, 120);
        assert(f[0].r == 27);
        f = rise_from_shown(st, f, dim, 40.0, 120);
        assert(f[0].r == 40);
        f = rise_from_shown(st, f, dim, 40.0, 120);
        assert(f[0].r == 40 && st.remaining_ms == 0.0);
    }
    // after a pause, however long: the same soft start from what is shown
    for (double gap : {100.1, 130.0, 480.0, 5000.0, 1e9, -5.0}) {
        RiseState st;
        assert(rise_from_shown(st, black, white, gap, 120)[0].r == 85);
        RiseState st2;
        auto h = rise_from_shown(st2, std::vector<RGB8>(5, RGB8{100, 100, 100}), white, gap, 120);
        assert(h[0].r == 152);  // 100 + ceil(155 / 3)
    }
    // falls stay immediate, also during a soft start; rise_ms 0 = no limit at all
    {
        RiseState st;
        assert(rise_from_shown(st, white, black, 5000.0, 120)[0].r == 0);
        RiseState st0;
        assert(rise_from_shown(st0, {}, white, 5000.0, 0)[0].r == 255 && st0.remaining_ms == 0.0);
    }
    // another LED count: a soft start; LEDs both frames have keep their value, new ones start black
    {
        RiseState st;
        auto more = rise_from_shown(st, std::vector<RGB8>(3, RGB8{200, 200, 200}), white, 40.0, 120);
        assert(more.size() == 5 && more[0].r == 219 && more[2].r == 219 && more[3].r == 85 && more[4].r == 85);
        RiseState st2;
        auto fewer = rise_from_shown(st2, std::vector<RGB8>(9, RGB8{200, 200, 200}), white, 40.0, 120);
        assert(fewer.size() == 5 && fewer[4].r == 219);
    }
    // never above the target, never below what was shown on a rise, and it always arrives
    for (int it = 0; it < 3000; ++it) {
        const size_t n = 1 + rng() % 30;
        std::vector<RGB8> p(rng() % 40), t(n);
        for (auto& c : p) c = {rnd8(), rnd8(), rnd8()};
        for (auto& c : t) c = {rnd8(), rnd8(), rnd8()};
        const double rise = rng() % 800;
        RiseState st;
        auto o = rise_from_shown(st, p, t, (rng() % 100000) / 10.0 - 100.0, rise);
        assert(o.size() == n);
        for (size_t i = 0; i < n; ++i) assert(o[i].r <= t[i].r && o[i].g <= t[i].g && o[i].b <= t[i].b);
        for (int k = 0; k < 40; ++k) o = rise_from_shown(st, o, t, 40.0, rise);
        for (size_t i = 0; i < n; ++i) assert(o[i].r == t[i].r && o[i].g == t[i].g && o[i].b == t[i].b);
    }
    printf("[ok] rise_from_shown: flowing frames = limit_rise; first frame and after a pause ramp in proportion over rise_ms (also low targets); LED count change\n");
}

// ---- v0.33: a held frame against a budget that changed meanwhile ---------------
static void test_fit_budget() {
    using color_pipeline::fit_budget;
    Settings s;
    s.led_count = 43;
    s.current_budget_ma = 800;
    const std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    const auto held = render_strip(white, s, nullptr, nullptr, 3.0);
    assert(estimate_current_ma(held, s) > 700.0);
    // nothing changed: returned as it is
    auto same = fit_budget(held, s);
    for (size_t i = 0; i < held.size(); ++i) assert(same[i].r == held[i].r && same[i].g == held[i].g && same[i].b == held[i].b);
    // the budget was lowered while the frame was held
    Settings low = s;
    low.current_budget_ma = 300;
    auto fitted = fit_budget(held, low);
    assert(estimate_current_ma(fitted, low) <= 300.0 && estimate_current_ma(fitted, low) > 270.0);
    for (size_t i = 0; i < held.size(); ++i) assert(fitted[i].r <= held[i].r);
    // mA per LED raised, budget below the idle draw, empty frame
    Settings more = s;
    more.ma_per_led_full_white = 120;
    assert(estimate_current_ma(fit_budget(held, more), more) <= 800.0);
    Settings none = s;
    none.current_budget_ma = 10;
    for (const auto& c : fit_budget(held, none)) assert(c.r == 0 && c.g == 0 && c.b == 0);
    assert(fit_budget({}, s).empty());
    std::mt19937 rng(4);
    for (int it = 0; it < 3000; ++it) {
        Settings x;
        x.current_budget_ma = rng() % 1200;
        x.ma_per_led_full_white = 20 + rng() % 60;
        x.idle_ma_per_led = (rng() % 20) / 10.0;
        std::vector<RGB8> f(1 + rng() % 60);
        for (auto& c : f) c = {uint8_t(rng() % 256), uint8_t(rng() % 256), uint8_t(rng() % 256)};
        auto o = fit_budget(f, x);
        const double floor_ma = x.idle_ma_per_led * f.size();
        assert(estimate_current_ma(o, x) <= std::max(color_pipeline::effective_budget_ma(x), floor_ma) + 1e-9);
    }
    printf("[ok] fit_budget: a held frame is cut to the budget of now (lowered budget, raised mA per LED), untouched when it fits\n");
}

// ---- v0.33: JointSpace side values -> LED levels --------------------------------
static void test_jointspace_curve() {
    using color_pipeline::jointspace_to_led;
    Settings d;
    assert(d.corner_js_curve == color_pipeline::kJsLinear && d.corner_js_full_scale == 23.0);
    // defaults = the expression main.cpp had until v0.32, for every possible value
    for (int v = 0; v < 256; ++v) {
        const uint8_t old = static_cast<uint8_t>(std::lround(255.0 * std::min(1.0, uint8_t(v) / 23.0)));
        RGB8 c = jointspace_to_led(uint8_t(v), 0, uint8_t(255 - v), d);
        assert(c.r == old && c.g == 0);
    }
    assert(jointspace_to_led(23, 12, 0, d).r == 255 && jointspace_to_led(23, 12, 0, d).g == 133);
    // srgb: the same curve as a Hue lamp value; ends unchanged, the middle much lower
    Settings g = d;
    g.corner_js_curve = color_pipeline::kJsSrgb;
    RGB8 mid = jointspace_to_led(12, 23, 0, g);
    assert(mid.g == 255 && mid.b == 0 && mid.r == srgb8_to_linear8(133) && mid.r < 70);
    for (int v = 0; v <= 23; ++v) assert(jointspace_to_led(uint8_t(v), 0, 0, g).r <= jointspace_to_led(uint8_t(v), 0, 0, d).r);
    // another full scale (another Ambilight brightness on the TV)
    Settings f = d;
    f.corner_js_full_scale = 77;
    assert(jointspace_to_led(77, 23, 200, f).r == 255 && jointspace_to_led(77, 23, 200, f).g == 76 &&
           jointspace_to_led(77, 23, 200, f).b == 255);
    // limits, names, JSON round trip
    Settings q;
    q.corner_js_curve = 7;
    q.corner_js_full_scale = 0.0;
    settings::sanitize(q);
    assert(q.corner_js_curve == color_pipeline::kJsLinear && q.corner_js_full_scale == 1.0);
    cJSON* j = settings::to_json(g);
    Settings back;
    settings::merge_json(back, j);
    cJSON_Delete(j);
    assert(back.corner_js_curve == color_pipeline::kJsSrgb && back.corner_js_full_scale == 23.0);
    // through the strip: the JointSpace edge of the tv layout follows the curve
    Settings t;
    t.led_count = 43;
    t.rise_ms = 0;
    t.edge_source = color_pipeline::kEdgeJointSpace;
    t.edge_strength = 1.0;
    const std::vector<RGB8> dark = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    Corners c;
    c.has_left = c.has_right = true;
    c.left = jointspace_to_led(12, 12, 12, t);
    c.right = c.left;
    auto lin = render_strip(dark, t, nullptr, &c);
    Settings ts = t;
    ts.corner_js_curve = color_pipeline::kJsSrgb;
    c.left = c.right = jointspace_to_led(12, 12, 12, ts);
    auto sr = render_strip(dark, ts, nullptr, &c);
    assert(lin[0].r == 133 && sr[0].r == srgb8_to_linear8(133) && sr[0].r < lin[0].r && lin[21].r == 0);
    printf("[ok] corner_js_curve: linear/23 identical to v0.32 for all 256 values, srgb = the lamps' curve, full scale, limits\n");
}

// ---- v0.33: wear-aware limiter ---------------------------------------------------
static void test_wear_aware() {
    using color_pipeline::wear_from_mah;
    // wear_from_mah: no data -> 0; flat use -> 0; a worn region -> up to 1, smooth
    assert(wear_from_mah({}, 43) == std::vector<double>(43, 0.0));
    assert(wear_from_mah(std::vector<double>(43, 0.0), 43) == std::vector<double>(43, 0.0));
    assert(wear_from_mah(std::vector<double>(43, 70.0), 43) == std::vector<double>(43, 0.0));
    std::vector<double> mah(43, 60.0);
    for (int i = 8; i <= 14; ++i) mah[i] = 90.0;  // +50 %: far over the 25 % that counts as full
    auto w = wear_from_mah(mah, 43);
    assert(w.size() == 43 && w[11] == 1.0 && w[30] == 0.0 && w[0] == 0.0);
    assert(w[6] > 0.0 && w[6] < w[7] && w[7] <= w[8]);  // 5-LED smoothing: a ramp, not a step
    for (double x : w) assert(x >= 0.0 && x <= 1.0);
    mah[20] = 60.0 * 1.10;  // one LED 10 % up: smoothed to 2 %, so barely worn
    assert(wear_from_mah(mah, 43)[20] > 0.0 && wear_from_mah(mah, 43)[20] < 0.15);
    assert(wear_from_mah(mah, 39).size() == 39 && wear_from_mah({80, 60}, 5).size() == 5);
    std::vector<double> bad(43, 60.0);
    bad[3] = std::nan("");
    bad[4] = -5.0;
    for (double x : wear_from_mah(bad, 43)) assert(x >= 0.0 && x <= 1.0);

    const std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    Settings s;
    s.led_count = 43;
    s.current_budget_ma = 450;
    s.peak_emphasis = 0.5;
    s.peak_drift_leds = 0.0;
    // strength 0 (default), or no data, or all-zero wear: byte for byte the same frame
    {
        assert(s.wear_strength == 0.0);
        RenderInfo i0, i1;
        auto a = render_strip(white, s, &i0, nullptr, 9.0);
        auto b = render_strip(white, s, &i1, nullptr, 9.0, &w);
        Settings on = s;
        on.wear_strength = 1.0;
        auto c = render_strip(white, on, nullptr, nullptr, 9.0, nullptr);
        std::vector<double> zero(43, 0.0), shorter(10, 1.0);
        auto d = render_strip(white, on, nullptr, nullptr, 9.0, &zero);
        auto e = render_strip(white, on, nullptr, nullptr, 9.0, &shorter);
        for (size_t i = 0; i < a.size(); ++i)
            assert(a[i].r == b[i].r && a[i].r == c[i].r && a[i].r == d[i].r && a[i].r == e[i].r && a[i].g == b[i].g && a[i].b == b[i].b);
        assert(!i0.wear_applied && !i1.wear_applied);
    }
    // strength > 0: the worn LEDs are dimmed more, the others get that current back; budget unchanged
    {
        Settings on = s;
        on.wear_strength = 0.6;
        RenderInfo info;
        auto a = render_strip(white, s, nullptr, nullptr, 9.0);
        auto b = render_strip(white, on, &info, nullptr, 9.0, &w);
        assert(info.wear_applied && std::string(info.limit_mode) == "peaks");
        assert(estimate_current_ma(b, on) <= 450.0 && estimate_current_ma(b, on) > 430.0);
        assert(b[11].r < a[11].r && b[30].r >= a[30].r && b[35].r > a[35].r);
        // within the budget nothing changes, whatever the wear
        Settings big = on;
        big.current_budget_ma = 900;
        big.ma_per_led_full_white = 10;
        auto x = render_strip(white, big, &info, nullptr, 9.0, &w);
        assert(std::string(info.limit_mode) == "none" && !info.wear_applied && x[11].r == 255);
        // pe = 0 never uses it
        Settings pe0 = on;
        pe0.peak_emphasis = 0.0;
        auto y0 = render_strip(white, pe0, nullptr, nullptr, 9.0, &w), y1 = render_strip(white, pe0, nullptr, nullptr, 9.0);
        for (size_t i = 0; i < y0.size(); ++i) assert(y0[i].r == y1[i].r);
    }
    // the budget holds with any wear vector
    std::mt19937 rng(61);
    for (int it = 0; it < 3000; ++it) {
        Settings x;
        x.led_count = 1 + rng() % 60;
        x.current_budget_ma = 30 + rng() % 900;
        x.peak_emphasis = (rng() % 5) / 4.0;
        x.peak_points = rng() % 6;
        x.wear_strength = (rng() % 11) / 10.0;
        x.strip_layout = rng() % 2;
        std::vector<double> wv(x.led_count);
        for (auto& v : wv) v = (rng() % 1300) / 1000.0 - 0.1;  // also out-of-range values
        std::vector<RGB8> lamps(rng() % 2 ? 3 : 5);
        for (auto& c : lamps) c = (rng() % 3 == 0) ? RGB8{255, 255, 255} : RGB8{uint8_t(rng() % 256), uint8_t(rng() % 256), uint8_t(rng() % 256)};
        auto f = render_strip(lamps, x, nullptr, nullptr, it * 0.37, &wv);
        const double floor_ma = x.idle_ma_per_led * f.size();
        assert(estimate_current_ma(f, x) <= std::max(x.current_budget_ma, floor_ma) + 1e-9);
    }
    // closed loop: hours of an over-budget white picture; the mAh spread ends up smaller with wear on
    auto spread_after = [&](double strength) {
        Settings x = s;
        x.wear_strength = strength;
        x.peak_drift_leds = 0.0;  // fixed peaks: the worst case for uneven wear
        std::vector<double> m(43, 0.0);
        for (int step = 0; step < 600; ++step) {
            auto wv = wear_from_mah(m, 43);
            auto f = render_strip(white, x, nullptr, nullptr, 1.0, &wv);
            for (int i = 0; i < 43; ++i) m[i] += color_pipeline::led_current_ma(f[i], x) * 60.0 / 3600.0;  // one minute each
        }
        double lo = 1e300, hi = 0.0;
        for (double v : m) lo = std::min(lo, v), hi = std::max(hi, v);
        return (hi - lo) / lo;
    };
    const double off = spread_after(0.0), on3 = spread_after(0.3), on10 = spread_after(1.0);
    assert(off > 0.5 && on3 < 0.8 * off && on10 < on3);
    // limits + JSON
    Settings q;
    q.wear_strength = 7.0;
    settings::sanitize(q);
    assert(q.wear_strength == 1.0);
    q.wear_strength = 0.3;
    cJSON* j = settings::to_json(q);
    Settings back;
    settings::merge_json(back, j);
    cJSON_Delete(j);
    assert(back.wear_strength == 0.3);
    printf("[ok] wear: 0 / no data identical; worn LEDs dimmed first, budget unchanged, pe=0 untouched; 10 h of white: mAh spread %.0f %% -> %.0f %% (0.3) -> %.0f %% (1.0)\n",
           100 * off, 100 * on3, 100 * on10);
}

// ---- v0.33: build-time ceiling of the current limit ------------------------
static void test_budget_ceiling() {
    assert(color_pipeline::kMaxBudgetMa == 900.0);  // this build's ceiling (cmake AMBI_MAX_BUDGET_MA)
    // a Settings struct that never went through sanitize() still cannot pass it
    const std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    for (double pe : {0.0, 0.5, 1.0}) {
        Settings s;
        s.led_count = 43;
        s.peak_emphasis = pe;
        s.current_budget_ma = 5000.0;
        RenderInfo info;
        auto out = render_strip(white, s, &info, nullptr, 12.0);
        assert(estimate_current_ma(out, s) <= 900.0);
        assert(std::string(info.limit_mode) != "none");
        Settings at = s;
        at.current_budget_ma = 900.0;
        auto same = render_strip(white, at, nullptr, nullptr, 12.0);
        for (size_t i = 0; i < out.size(); ++i) assert(out[i].r == same[i].r && out[i].g == same[i].g && out[i].b == same[i].b);
    }
    // sanitize: above the ceiling -> the ceiling; at or below it -> untouched (a saved 800 stays 800)
    Settings s;
    s.current_budget_ma = 20000.0;
    settings::sanitize(s);
    assert(s.current_budget_ma == 900.0);
    for (double keep : {0.0, 450.0, 800.0, 900.0}) {
        Settings k;
        k.current_budget_ma = keep;
        settings::sanitize(k);
        assert(k.current_budget_ma == keep);
        cJSON* j = settings::to_json(k);
        Settings back;
        settings::merge_json(back, j);
        cJSON_Delete(j);
        assert(back.current_budget_ma == keep);
    }
    printf("[ok] budget ceiling %.0f mA: holds without sanitize(), sanitize clamps only above it, 800 stays 800\n",
           color_pipeline::kMaxBudgetMa);
}

static void test_rise_limit() {
    std::vector<RGB8> black(5, RGB8{0, 0, 0}), white(5, RGB8{255, 255, 255});
    // 0 -> 255 in 120 ms at 40 ms frames: 85, 170, 255
    auto a = limit_rise(black, white, 40, 120);
    assert(a[0].r == 85);
    auto b = limit_rise(a, white, 40, 120);
    assert(b[0].r == 170);
    auto c = limit_rise(b, white, 40, 120);
    assert(c[0].r == 255);
    // falls are immediate
    auto d = limit_rise(white, black, 40, 120);
    assert(d[0].r == 0);
    // mixed: one channel up (limited), one down (immediate)
    auto e = limit_rise({{0, 200, 0}}, {{255, 10, 0}}, 40, 120);
    assert(e[0].r == 85 && e[0].g == 10);
    // never above target, so never above the budget of the target frame
    for (int it = 0; it < 500; ++it) {
        std::vector<RGB8> p = random_lamps(7), t = random_lamps(7);
        auto o = limit_rise(p, t, rnd() % 100, 1 + rnd() % 400);
        for (int i = 0; i < 7; ++i) assert(o[i].r <= t[i].r && o[i].g <= t[i].g && o[i].b <= t[i].b);
    }
    // off / gaps / size change: target unchanged
    assert(limit_rise(black, white, 40, 0)[0].r == 255);
    assert(limit_rise(black, white, 1000, 120)[0].r == 255);
    assert(limit_rise(std::vector<RGB8>(3), white, 40, 120).size() == 5);
    // very slow rise still moves (step < 1 per frame)
    assert(limit_rise(black, white, 10, 100000)[0].r == 1);
    printf("[ok] rise limit: 0->255 in 3 frames at 120 ms, immediate fall, never above target\n");
}

static void test_settings_roundtrip_and_corrupt_file() {
    const std::string path = "/tmp/ambi_test_settings.json";
    Settings s;
    s.led_count = 27;
    s.diffuser = true;
    s.current_budget_ma = 500.0;
    s.peak_emphasis = 0.7;
    s.peak_points = 5;
    s.rise_ms = 200.0;
    assert(settings::save(s, path));

    Settings loaded = settings::load(path);
    assert(loaded.led_count == 27);
    assert(loaded.diffuser == true);
    assert(loaded.current_budget_ma == 500.0);
    assert(loaded.peak_emphasis == 0.7 && loaded.peak_points == 5 && loaded.rise_ms == 200.0);

    // Out-of-range values are clamped; a pre-v0.13 file (no new keys) keeps the defaults.
    FILE* g = std::fopen(path.c_str(), "wb");
    const char* js = "{\"led_count\": 43, \"peak_emphasis\": 7, \"rise_ms\": -5}";
    std::fwrite(js, 1, std::strlen(js), g);
    std::fclose(g);
    Settings cl = settings::load(path);
    assert(cl.peak_emphasis == 1.0 && cl.rise_ms == 0.0 && cl.peak_points == Settings{}.peak_points);

    // Corrupt file must yield safe defaults, not a crash.
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite("{not valid json", 1, 15, f);
    std::fclose(f);
    Settings fallback = settings::load(path);
    assert(fallback.led_count == Settings{}.led_count);
    printf("[ok] settings save/load roundtrip + corrupt-file fallback\n");
}

static void test_hex_roundtrip() {
    std::vector<uint8_t> bytes = {0x00, 0x01, 0x7f, 0x80, 0xff, 0xab, 0xcd};
    std::string hex = pairing_store::hex_encode(bytes);
    assert(hex == "00017f80ffabcd");
    auto back = pairing_store::hex_decode(hex);
    assert(back == bytes);
    assert(pairing_store::hex_decode("abc").empty());    // odd length -> empty, never half a byte
    assert(pairing_store::hex_decode("zz01").empty());   // bad nibble -> empty, never garbage
    printf("[ok] hex encode/decode roundtrip + malformed-input safety\n");
}

static void test_pairing_store_pair_and_persist() {
    const std::string path = "/tmp/ambi_test_pairing.json";
    std::remove(path.c_str());

    pairing_store::Store s = pairing_store::Store::load(path);  // no file yet -> empty
    assert(s.users().empty());

    bool changed = false;
    auto [user1, u1] = s.pair("philips_hue_entertainment#tv1", /*generate_clientkey=*/true, changed);
    assert(changed);
    assert(user1.size() == 32);           // 16 random bytes, hex-encoded
    assert(u1.clientkey_hex.size() == 32);
    assert(u1.devicetype == "philips_hue_entertainment#tv1");

    // Re-pairing the same devicetype returns the SAME user/clientkey, not a new one -
    // this is what lets the TV reconnect after a reboot without re-pairing each time.
    bool changed2 = false;
    auto [user2, u2] = s.pair("philips_hue_entertainment#tv1", /*generate_clientkey=*/true, changed2);
    assert(!changed2);
    assert(user2 == user1);
    assert(u2.clientkey_hex == u1.clientkey_hex);

    auto psk = s.psk_for(user1);
    assert(psk.size() == 16);
    assert(pairing_store::hex_encode(psk) == u1.clientkey_hex);
    assert(s.psk_for("no-such-user").empty());

    assert(s.save(path));
    pairing_store::Store loaded = pairing_store::Store::load(path);
    auto got = loaded.get(user1);
    assert(got.has_value());
    assert(got->clientkey_hex == u1.clientkey_hex);
    assert(got->devicetype == u1.devicetype);

    // Corrupt file must yield an empty store, not a crash.
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite("{not valid json", 1, 15, f);
    std::fclose(f);
    pairing_store::Store fallback = pairing_store::Store::load(path);
    assert(fallback.users().empty());
    printf("[ok] pairing store: pair/re-pair/persist/corrupt-file fallback\n");
}

static void test_bridge_identity_derivation() {
    // Cross-checked against fakebridge.py's Ident class for the user's own
    // fixed MAC/name: bridgeid = mac[:6] + "FFFE" + mac[6:] (uppercase, no colons).
    auto id = bridge_identity::make_identity("192.0.2.50", 80);
    assert(id.mac == "00:17:88:a4:5e:01");
    assert(id.serial == "001788a45e01");
    assert(id.bridgeid == "001788FFFEA45E01");
    assert(id.udn == "2f402f80-da50-11e1-9b23-001788a45e01");
    std::string xml = bridge_identity::description_xml(id);
    assert(xml.find("<serialNumber>001788a45e01</serialNumber>") != std::string::npos);
    assert(xml.find("URLBase>http://192.0.2.50:80/</URLBase") != std::string::npos);
    printf("[ok] bridge identity derivation matches fakebridge.py's Ident\n");
}

// v0.15 (Phase 3): the same limits for every way a setting arrives.
static void test_settings_merge_and_sanitize() {
    Settings s;  // defaults
    cJSON* j = cJSON_Parse(R"({"led_count":5000,"current_budget_ma":-50,"ma_per_led_full_white":0,
        "idle_ma_per_led":99,"peak_emphasis":2,"rise_ms":1e9,"peak_points":-3,"min_anchors":1e12,
        "diffuser":"yes","unknown_key":1})");
    assert(j);
    settings::merge_json(s, j);
    cJSON_Delete(j);
    assert(s.led_count == 1000);
    assert(s.current_budget_ma == 0.0);
    assert(s.ma_per_led_full_white == 1.0);  // never 0: it divides the budget
    assert(s.idle_ma_per_led == 20.0);
    assert(s.peak_emphasis == 1.0);
    assert(s.rise_ms == 10000.0);
    assert(s.peak_points == 0);
    assert(s.min_anchors == 1000);           // 1e12 must not overflow the int
    assert(s.diffuser == Settings{}.diffuser);  // wrong type ignored

    // Partial merge: only the keys sent change; wrong types change nothing.
    Settings p;
    cJSON* j2 = cJSON_Parse(R"({"ma_per_led_full_white":38.5,"led_count":"43x","rise_ms":null})");
    settings::merge_json(p, j2);
    cJSON_Delete(j2);
    assert(p.ma_per_led_full_white == 38.5);
    assert(p.led_count == Settings{}.led_count);
    assert(p.rise_ms == Settings{}.rise_ms);
    assert(p.current_budget_ma == 450.0);  // v0.32 default (was 800 since v0.14)

    // Non-JSON-object input: only sanitize runs.
    Settings q;
    q.current_budget_ma = std::nan("");
    settings::merge_json(q, nullptr);
    assert(q.current_budget_ma == Settings{}.current_budget_ma);

    // to_json -> merge_json round trip is exact.
    Settings r;
    r.led_count = 60; r.current_budget_ma = 834; r.ma_per_led_full_white = 41.2; r.idle_ma_per_led = 0.7;
    r.peak_emphasis = 0.3; r.rise_ms = 250; r.peak_points = 7; r.diffuser = true;
    cJSON* rj = settings::to_json(r);
    Settings back;
    settings::merge_json(back, rj);
    cJSON_Delete(rj);
    assert(back.led_count == 60 && back.current_budget_ma == 834 && back.ma_per_led_full_white == 41.2 &&
           back.idle_ma_per_led == 0.7 && back.peak_emphasis == 0.3 && back.rise_ms == 250 &&
           back.peak_points == 7 && back.diffuser);
    // v0.24: "auto" drift, shape, end_lamps, stream-loss times
    {
        Settings v;
        cJSON* j = settings::to_json(v);
        cJSON* dl = cJSON_GetObjectItemCaseSensitive(j, "peak_drift_leds");
        assert(cJSON_IsString(dl) && std::string(dl->valuestring) == "auto");
        cJSON_Delete(j);
        cJSON* in = cJSON_Parse("{\"peak_drift_leds\":7,\"peak_drift_shape\":\"sine\",\"end_lamps\":true,"
                                "\"loss_hold_ms\":-5,\"loss_fade_ms\":400,\"loss_black_s\":99999}");
        settings::merge_json(v, in);
        cJSON_Delete(in);
        assert(v.peak_drift_leds == 7 && v.peak_drift_shape == kDriftSine && v.end_lamps);
        assert(v.loss_hold_ms == 0 && v.loss_fade_ms == 400 && v.loss_black_s == 3600);
        cJSON* in2 = cJSON_Parse("{\"peak_drift_leds\":\"auto\",\"peak_drift_shape\":\"zigzag\"}");
        settings::merge_json(v, in2);
        cJSON_Delete(in2);
        assert(v.peak_drift_leds == -1.0 && v.peak_drift_shape == kDriftSine);  // unknown shape: kept
        cJSON* in3 = cJSON_Parse("{\"peak_drift_leds\":-7.5}");
        settings::merge_json(v, in3);
        cJSON_Delete(in3);
        assert(v.peak_drift_leds == -1.0);
        // a v0.23 file (no new keys) keeps its own drift numbers and gets the new keys' defaults
        Settings old;
        cJSON* in4 = cJSON_Parse("{\"peak_drift_leds\":7,\"peak_drift_period_s\":60}");
        settings::merge_json(old, in4);
        cJSON_Delete(in4);
        assert(old.peak_drift_leds == 7 && old.peak_drift_period_s == 60 && old.peak_drift_shape == kDriftTriangle &&
               !old.end_lamps && old.loss_hold_ms == 1000);
    }
    printf("[ok] settings merge_json/sanitize clamp, ignore wrong types, round trip (+ v0.24 keys)\n");
}

// v0.15: "the TV wants to pair" state in the pairing store.
static void test_pair_request_state() {
    std::string path = "/tmp/ambi_test_pairreq_" + std::to_string(::getpid()) + ".json";
    std::remove(path.c_str());
    pairing_store::ThreadSafeStore st(path);
    auto a = st.pair_request_state();
    assert(!a.tv_waiting && a.refused_ago_ms == -1 && a.paired_ago_ms == -1);
    st.note_pair_refused();
    auto b = st.pair_request_state();
    assert(b.tv_waiting && b.refused_ago_ms >= 0);
    // window over -> no longer waiting
    assert(!st.pair_request_state(std::chrono::milliseconds(0)).tv_waiting);
    st.pair("Philips TV#test", true);
    auto c = st.pair_request_state();
    assert(!c.tv_waiting && c.paired_ago_ms >= 0);  // paired after the refusal
    st.note_pair_refused();                          // a new refusal after that
    assert(st.pair_request_state().tv_waiting);
    std::remove(path.c_str());
    printf("[ok] pair_request_state: refused -> waiting, paired -> cleared, window expires\n");
}

// v0.16: lamp positions on the strip (lamp_inset).
static void test_lamp_inset() {
    // Quarters (default 0.25): 3 lamps at 25/50/75 %; outside them the outer
    // lamp's colour holds; in between linear.
    std::vector<RGB8> lamps = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    Settings s = legacy_layout();  // default lamp_inset
    assert(s.lamp_inset == 0.25);
    s.led_count = 41;  // LED i at i/40: 10 = 25 %, 20 = 50 %, 30 = 75 %
    no_limit(s);
    auto leds = render_strip(lamps, s, static_cast<int*>(nullptr));
    for (int i = 0; i <= 10; ++i) assert(leds[i].r == 255 && leds[i].g == 0 && leds[i].b == 0);
    assert(leds[20].g == 255 && leds[20].r == 0 && leds[20].b == 0);
    for (int i = 30; i <= 40; ++i) assert(leds[i].b == 255 && leds[i].r == 0 && leds[i].g == 0);
    assert(leds[15].r > 100 && leds[15].g > 100);  // halfway red -> green

    auto pos = lamp_positions(3, 0.25);
    assert(pos.size() == 3 && std::fabs(pos[0] - 0.25) < 1e-12 && pos[1] == 0.5 && std::fabs(pos[2] - 0.75) < 1e-12);
    assert(lamp_positions(1, 0.25).size() == 1 && lamp_positions(1, 0.25)[0] == 0.5);
    auto ends = lamp_positions(3, 0.0);
    assert(ends[0] == 0.0 && ends[2] == 1.0);

    // Peaks follow the lamps: 43 LEDs -> 25 % = LED 10.5 -> 11 (lround), 21, 32.
    Settings p = legacy_layout();
    p.peak_emphasis = 0.5;
    std::vector<int> peaks;
    peak_weights(43, 3, p, &peaks);
    assert(peaks.size() == 3 && peaks[0] == 11 && peaks[1] == 21 && peaks[2] == 32);
    p.lamp_inset = 0.0;
    peak_weights(43, 3, p, &peaks);
    assert(peaks[0] == 0 && peaks[1] == 21 && peaks[2] == 42);

    // 5 lamps, quarters: 25, 37.5, 50, 62.5, 75 %.
    auto p5 = lamp_positions(5, 0.25);
    assert(std::fabs(p5[1] - 0.375) < 1e-12 && std::fabs(p5[3] - 0.625) < 1e-12);

    // Clamped like every other setting.
    Settings c = legacy_layout();
    c.lamp_inset = 0.9;
    settings::sanitize(c);
    assert(c.lamp_inset == 0.45);
    printf("[ok] lamp_inset: quarters by default, colours hold outside, peaks follow, 0 = old ends\n");
}

// v0.17: identities (MAC, name, lamps) with their own pairing dirs.
static void test_identity_store() {
    using namespace identity_store;
    std::string dir = "/tmp/ambi_test_ids_" + std::to_string(::getpid());
    std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    // Nothing saved -> the old fixed default, pairing in the state dir itself.
    Store s = load(dir);
    assert(s.entries.size() == 1 && s.active == "00:17:88:a4:5e:01" && s.entries[0].name == "FakeBridge-A45E01");
    assert(s.entries[0].lamps == 3);
    assert(dir_for(dir, "00:17:88:A4:5E:01") == dir);
    assert(dir_for(dir, "00:17:88:01:02:03") == dir + "/identities/001788010203");
    // Names: ASCII, 1-32, no XML-special characters, no edge spaces.
    assert(valid_name("Living room TV", nullptr));
    std::string err;
    assert(!valid_name("", &err) && err == "name_empty");
    assert(!valid_name(std::string(33, 'a'), &err) && err == "name_too_long");
    assert(!valid_name("Σαλόνι", &err) && err == "name_not_ascii");
    assert(!valid_name("a<b", &err) && err == "name_bad_character");
    assert(!valid_name(" a", &err) && err == "name_spaces_at_ends");
    assert(valid_mac("00:17:88:AB:cd:01") && !valid_mac("00:17:88:ab:cd") && !valid_mac("00-17-88-ab-cd-01"));
    assert(default_name_for("00:17:88:ab:cd:01") == "FakeBridge-ABCD01");
    std::string m = random_mac();
    assert(valid_mac(m) && m.rfind("00:17:88:", 0) == 0);

    Shared sh(dir, s.entries[0]);
    std::string mac;
    assert(sh.create("", &mac, &err));
    Store a = sh.snapshot();
    assert(a.entries.size() == 2 && a.active == mac && a.find(mac)->name == default_name_for(mac));
    assert(sh.rename(mac, "Test Bridge", &err));
    assert(sh.set_lamps(mac, 5, &err));
    assert(!sh.set_lamps(mac, 4, &err) && err == "lamps_must_be_3_or_5");
    assert(!sh.rename(mac, "bad&name", &err));
    assert(!sh.select("00:17:88:00:00:99", &err) && err == "unknown_identity");
    // Persisted, and reloads the same.
    Store b = load(dir);
    assert(b.active == mac && b.find(mac)->name == "Test Bridge" && b.find(mac)->lamps == 5);
    assert(sh.select("00:17:88:a4:5e:01", &err));
    assert(load(dir).active == "00:17:88:a4:5e:01");
    // Corrupt file -> default, never a crash; the default is always kept.
    { FILE* f = std::fopen((dir + "/identity.json").c_str(), "wb"); std::fputs("{nope", f); std::fclose(f); }
    Store c = load(dir);
    assert(c.entries.size() == 1 && c.active == "00:17:88:a4:5e:01");
    { FILE* f = std::fopen((dir + "/identity.json").c_str(), "wb");
      std::fputs(R"({"active":"00:17:88:01:02:03","identities":[{"mac":"00:17:88:01:02:03","name":"x<y","lamps":7}]})", f);
      std::fclose(f); }
    Store d = load(dir);
    assert(d.entries.size() == 2 && d.active == "00:17:88:01:02:03");
    assert(d.find("00:17:88:01:02:03")->name == "FakeBridge-010203" && d.find("00:17:88:01:02:03")->lamps == 3);
    std::system(("rm -rf " + dir).c_str());
    printf("[ok] identity store: default, names/MACs validated, create/rename/lamps/select persist, corrupt file safe\n");
}

// v0.18: blend_mode (smooth / zones / zones_mid) and corner_mode (lamp / side).
static void test_blend_and_corners() {
    std::vector<RGB8> lamps = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    Settings s = legacy_layout();
    s.led_count = 41;            // LED i at i/40: 10 = 25 %, 20 = 50 %, 30 = 75 %
    no_limit(s);
    s.corner_mode = kCornerLamp;
    auto R = [](RGB8 c) { return c.r == 255 && c.g == 0 && c.b == 0; };
    auto G = [](RGB8 c) { return c.r == 0 && c.g == 255 && c.b == 0; };
    auto B = [](RGB8 c) { return c.r == 0 && c.g == 0 && c.b == 255; };

    // Zones: hard switch halfway between lamps (37.5 % = LED 15, 62.5 % = 25).
    s.blend_mode = kBlendZones;
    auto z = render_strip(lamps, s, static_cast<int*>(nullptr));
    for (int i = 0; i <= 15; ++i) assert(R(z[i]));
    for (int i = 16; i <= 25; ++i) assert(G(z[i]));
    for (int i = 26; i <= 40; ++i) assert(B(z[i]));

    // Zones + in-between: R | R+G | G | G+B | B, each in-between zone centred on 37.5 / 62.5 %.
    s.blend_mode = kBlendZonesMid;
    auto m = render_strip(lamps, s, static_cast<int*>(nullptr));
    for (int i = 0; i <= 12; ++i) assert(R(m[i]));
    for (int i = 13; i <= 17; ++i) assert(m[i].r == 128 && m[i].g == 128 && m[i].b == 0);
    for (int i = 18; i <= 22; ++i) assert(G(m[i]));
    for (int i = 23; i <= 27; ++i) assert(m[i].r == 0 && m[i].g == 128 && m[i].b == 128);
    for (int i = 28; i <= 40; ++i) assert(B(m[i]));

    // Smooth with lamp corners: unchanged from v0.16 (held outside, gradient inside).
    s.blend_mode = kBlendSmooth;
    Corners c;
    c.has_left = c.has_right = true;
    c.left = {255, 255, 0};   // yellow side on the left
    c.right = {255, 255, 255};
    auto plain = render_strip(lamps, s, nullptr, &c);  // corner_mode lamp: corners ignored
    Settings s16 = s;
    auto v16 = render_strip(lamps, s16, static_cast<int*>(nullptr));
    for (int i = 0; i < 41; ++i) assert(plain[i].r == v16[i].r && plain[i].g == v16[i].g && plain[i].b == v16[i].b);

    // Side corners, smooth: LED 0 = average(left side, red) = (255,128,0),
    // blending to pure red at the lamp (LED 10); right end = average(blue, white).
    s.corner_mode = kCornerSide;
    auto sd = render_strip(lamps, s, nullptr, &c);
    assert(sd[0].r == 255 && sd[0].g == 128 && sd[0].b == 0);
    assert(sd[5].r == 255 && sd[5].g > 0 && sd[5].g < 128);
    assert(R(sd[10]) && G(sd[20]) && B(sd[30]));
    assert(sd[40].r == 128 && sd[40].g == 128 && sd[40].b == 255);
    // Side corners, zones: the corner zone (0 .. 12.5 %) takes the mix, then red.
    s.blend_mode = kBlendZones;
    auto sz = render_strip(lamps, s, nullptr, &c);
    for (int i = 0; i <= 5; ++i) assert(sz[i].r == 255 && sz[i].g == 128 && sz[i].b == 0);
    for (int i = 6; i <= 15; ++i) assert(R(sz[i]));
    // Only one side live: the other end holds its lamp.
    Corners one;
    one.has_left = true;
    one.left = {255, 255, 0};
    s.blend_mode = kBlendSmooth;
    auto so = render_strip(lamps, s, nullptr, &one);
    assert(so[0].g == 128 && B(so[40]));
    // No JointSpace at all -> same as lamp corners.
    auto sn = render_strip(lamps, s, nullptr, nullptr);
    for (int i = 0; i < 41; ++i) assert(sn[i].r == v16[i].r && sn[i].g == v16[i].g && sn[i].b == v16[i].b);
    // inset 0 leaves no room for a corner: corners ignored.
    s.lamp_inset = 0.0;
    auto s0 = render_strip(lamps, s, nullptr, &c);
    assert(R(s0[0]) && B(s0[40]));

    // The budget stays hard in every mode, with and without corners (pe 0 and 0.5).
    for (int bm = 0; bm <= 2; ++bm)
        for (int cm = 0; cm <= 1; ++cm)
            for (double pe : {0.0, 0.5}) {
                Settings h = legacy_layout();
                h.blend_mode = bm;
                h.corner_mode = cm;
                h.peak_emphasis = pe;
                std::vector<RGB8> white = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
                auto o = render_strip(white, h, nullptr, &c);
                assert(o.size() == 43 && estimate_current_ma(o, h) <= h.current_budget_ma + 1e-9);
            }

    // Settings round trip as strings; unknown strings ignored.
    Settings j = legacy_layout();
    cJSON* in = cJSON_Parse(R"({"blend_mode":"zones_mid","corner_mode":"lamp"})");
    settings::merge_json(j, in);
    cJSON_Delete(in);
    assert(j.blend_mode == kBlendZonesMid && j.corner_mode == kCornerLamp);
    cJSON* bad = cJSON_Parse(R"({"blend_mode":"wavy","corner_mode":3})");
    settings::merge_json(j, bad);
    cJSON_Delete(bad);
    assert(j.blend_mode == kBlendZonesMid && j.corner_mode == kCornerLamp);
    cJSON* out = settings::to_json(j);
    assert(std::string(cJSON_GetObjectItem(out, "blend_mode")->valuestring) == "zones_mid");
    assert(std::string(cJSON_GetObjectItem(out, "corner_mode")->valuestring) == "lamp");
    cJSON_Delete(out);
    Settings dflt = legacy_layout();
    assert(dflt.blend_mode == kBlendSmooth && dflt.corner_mode == kCornerSide);
    printf("[ok] blend smooth/zones/zones_mid + corners lamp/side, budget hard in all 12 combinations\n");
}


// v0.19: which SSDP searches are answered, and the who-asks registry.
// ---- v0.30: tv layout (bottom lamps end to end, edge colour), ring peaks ----
static bool same_rgb(const RGB8& a, const RGB8& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static RGB8 mix8(const RGB8& a, const RGB8& b, double w) {  // a towards b by w, rounded like the pipeline
    auto c = [w](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(x + (double(y) - x) * w))));
    };
    return {c(a.r, b.r), c(a.g, b.g), c(a.b, b.b)};
}
// largest gap between peaks on the strip, counting both ends
static double max_peak_gap(const std::vector<int>& peaks, int leds) {
    if (peaks.empty()) return leds;
    double g = std::max<double>(peaks.front(), (leds - 1) - peaks.back());
    for (size_t i = 1; i < peaks.size(); ++i) g = std::max<double>(g, peaks[i] - peaks[i - 1]);
    return g;
}

static void test_v030_layout() {
    // defaults
    Settings d;
    assert(d.strip_layout == kLayoutTv && d.edge_leds == 3 && d.edge_strength == 0.5 && d.edge_source == kEdgeAuto);
    assert(!end_lamps_active([] { Settings e; e.end_lamps = true; return e; }(), 5));  // inset-only feature
    // which lamps are the bottom, and where
    {
        auto b5 = bottom_lamp_indices(d, 5), b3 = bottom_lamp_indices(d, 3);
        assert(b5 == std::vector<int>({1, 2, 3}) && b3 == std::vector<int>({0, 1, 2}));
        auto p5 = strip_lamp_positions(d, 5), p3 = strip_lamp_positions(d, 3);
        assert(p5.size() == 3 && p5[0] == 0.0 && p5[1] == 0.5 && p5[2] == 1.0 && p3 == p5);
        assert(strip_lamp_positions(d, 1) == std::vector<double>({0.5}));
    }
    // edge source
    {
        Settings j = d;
        assert(edge_source_resolved(d, 5) == kEdgeLamps && edge_source_resolved(d, 3) == kEdgeJointSpace);
        j.edge_source = kEdgeLamps;
        assert(edge_source_resolved(j, 3) == kEdgeNone);  // lamps asked, only 3: nothing
        j.edge_source = kEdgeJointSpace;
        assert(edge_source_resolved(j, 5) == kEdgeJointSpace);
        j.edge_leds = 0;
        assert(edge_source_resolved(j, 5) == kEdgeNone);
        assert(!wants_side_colours(d, 5) && wants_side_colours(d, 3));
        Settings in = legacy_layout();
        assert(edge_source_resolved(in, 5) == kEdgeNone && wants_side_colours(in, 5) == (in.corner_mode == kCornerSide));
    }
    // colours: 5 lamps, dim (under budget), smooth
    const RGB8 L1{100, 0, 0}, L2{0, 60, 0}, L3{0, 0, 60}, L4{60, 60, 0}, L5{0, 60, 60};
    const std::vector<RGB8> five{L1, L2, L3, L4, L5};
    {
        Settings s = d;
        s.edge_leds = 0;
        RenderInfo info;
        auto out = render_strip(five, s, &info);
        assert(out.size() == 43 && std::string(info.limit_mode) == "none");
        assert(same_rgb(out[0], L2) && same_rgb(out[21], L3) && same_rgb(out[42], L4));  // lamps 1 and 5 not used
        assert(same_rgb(out[10], mix8(L2, L3, 10.0 / 21.0)) && same_rgb(out[32], mix8(L3, L4, 11.0 / 21.0)));
        // edge colour from lamps 1 / 5: 0.5, 1/3, 1/6 at LEDs 0, 1, 2; LED 3 untouched
        Settings e = d;
        auto oe = render_strip(five, e, &info);
        assert(same_rgb(oe[0], mix8(L2, L1, 0.5)) && same_rgb(oe[42], mix8(L4, L5, 0.5)));
        assert(same_rgb(oe[1], mix8(out[1], L1, 0.5 * 2 / 3)) && same_rgb(oe[2], mix8(out[2], L1, 0.5 / 3)));
        assert(same_rgb(oe[3], out[3]) && same_rgb(oe[39], out[39]) && same_rgb(oe[21], out[21]));
        // strength 1: the end LED IS the side lamp
        e.edge_strength = 1.0;
        oe = render_strip(five, e, &info);
        assert(same_rgb(oe[0], L1) && same_rgb(oe[42], L5));
        // jointspace source with 5 lamps: corners, not lamps 1 / 5
        Settings js = d;
        js.edge_source = kEdgeJointSpace;
        Corners c;
        c.has_left = c.has_right = true;
        c.left = {0, 0, 200};
        c.right = {200, 0, 0};
        auto oj = render_strip(five, js, &info, &c);
        assert(same_rgb(oj[0], mix8(L2, c.left, 0.5)) && same_rgb(oj[42], mix8(L4, c.right, 0.5)));
        c.weight = 0.5;  // JointSpace going stale: half the share
        oj = render_strip(five, js, &info, &c);
        assert(same_rgb(oj[0], mix8(L2, c.left, 0.25)));
        auto none = render_strip(five, js, &info, nullptr);  // no JointSpace: plain lamps
        assert(same_rgb(none[0], L2));
        // 3 lamps: all bottom, edges from JointSpace (auto)
        const std::vector<RGB8> three{L2, L3, L4};
        c.weight = 1.0;
        auto o3 = render_strip(three, d, &info, &c);
        assert(same_rgb(o3[0], mix8(L2, c.left, 0.5)) && same_rgb(o3[21], L3) && same_rgb(o3[42], mix8(L4, c.right, 0.5)));
        // zones: nearest of 0 / 50 / 100 %
        Settings z = d;
        z.edge_leds = 0;
        z.blend_mode = kBlendZones;
        auto oz = render_strip(five, z, &info);
        assert(same_rgb(oz[10], L2) && same_rgb(oz[11], L3) && same_rgb(oz[31], L3) && same_rgb(oz[32], L4));
        // edge_leds larger than half the strip: clamped, no crash
        Settings big = d;
        big.led_count = 4;
        big.edge_leds = 10;
        assert(render_strip(five, big, &info).size() == 4);
    }
    // ring peaks (koryfes.png): evenly spread at every drift phase, every LED gets its turn
    {
        Settings s = d;  // 43 LEDs, 3 peaks (one per bottom lamp), auto drift, triangle
        assert(std::fabs(peak_drift_amplitude(s, 5) - 43.0 / 6.0) < 1e-9);
        const double spacing = 43.0 / 3.0;
        double worst = 0;
        std::vector<int> hit(43, 0);
        for (int k = 0; k <= 3000; ++k) {
            double t = s.peak_drift_period_s * k / 3000.0;
            std::vector<int> peaks;
            peak_weights(43, 5, s, &peaks, peak_drift_offset(s, t, 5));
            assert(peaks.size() == 3);
            worst = std::max(worst, max_peak_gap(peaks, 43));
            for (int p : peaks) hit[p] = 1;
        }
        int covered = 0;
        for (int h : hit) covered += h;
        assert(worst <= spacing + 1.0 && covered == 43);
        // the v0.29 (inset + end_lamps) behaviour this replaces: one half of the strip had no peak
        Settings old = legacy_layout();
        old.end_lamps = true;
        double old_worst = 0;
        for (int k = 0; k <= 3000; ++k) {
            std::vector<int> peaks;
            peak_weights(43, 5, old, &peaks, peak_drift_offset(old, old.peak_drift_period_s * k / 3000.0, 5));
            old_worst = std::max(old_worst, max_peak_gap(peaks, 43));
        }
        assert(old_worst >= 20.0);
        // ring symmetry: shifting by one spacing gives the same weights; peak_points N = N peaks
        auto w0 = peak_weights(43, 5, s, nullptr, 0.0), w1 = peak_weights(43, 5, s, nullptr, spacing);
        for (int i = 0; i < 43; ++i) assert(std::fabs(w0[i] - w1[i]) < 1e-9);
        Settings p4 = s;
        p4.peak_points = 4;
        std::vector<int> pk;
        peak_weights(43, 5, p4, &pk, 0.0);
        assert(pk.size() == 4 && max_peak_gap(pk, 43) <= 43.0 / 4 + 1);
        auto c = ring_peak_centres(43, 3, 0.0);
        assert(std::fabs(c[0] - (43.0 / 6 - 0.5)) < 1e-9 && std::fabs(c[1] - 21.0) < 1e-9 &&
               std::fabs((42.0 - c[2]) - c[0]) < 1e-9);  // symmetric round the middle LED
        c = ring_peak_centres(43, 3, -10.0);  // wraps: still 3, still evenly spaced
        assert(c.size() == 3 && c[0] >= -0.5 && c[2] < 42.5);
        assert(std::fabs(std::fmod(c[1] - c[0] + 43, 43) - spacing) < 1e-9);
        // an over-budget white frame really keeps 3 peaks
        RenderInfo info;
        std::vector<RGB8> white(5, RGB8{255, 255, 255});
        auto out = render_strip(white, s, &info, nullptr, 0.0);
        assert(std::string(info.limit_mode) == "peaks" && info.peak_leds.size() == 3 &&
               estimate_current_ma(out, s) <= s.current_budget_ma);
    }
    // budget stays a hard limit in the tv layout
    int checked = 0;
    for (int n = 0; n < 2000; ++n) {
        Settings x;
        x.led_count = 1 + rnd() % 80;
        x.current_budget_ma = 30 + rnd() % 2500;
        x.peak_emphasis = (rnd() % 3 == 0) ? 0.0 : (rnd() % 101) / 100.0;
        x.peak_points = (rnd() % 3 == 0) ? int(1 + rnd() % 8) : 0;
        x.blend_mode = int(rnd() % 3);
        x.blend_softness = (rnd() % 101) / 100.0;
        x.edge_leds = int(rnd() % 11);
        x.edge_strength = (rnd() % 101) / 100.0;
        x.edge_source = int(rnd() % 3);
        x.peak_drift_leds = (rnd() % 2) ? -1.0 : double(rnd() % 12);
        settings::sanitize(x);
        int nl = (rnd() % 2) ? 5 : 3;
        std::vector<RGB8> lamps;
        for (int j = 0; j < nl; ++j) lamps.push_back({uint8_t(rnd() % 256), uint8_t(rnd() % 256), uint8_t(rnd() % 256)});
        Corners c;
        c.has_left = rnd() % 2;
        c.has_right = rnd() % 2;
        c.left = {uint8_t(rnd() % 256), uint8_t(rnd() % 256), uint8_t(rnd() % 256)};
        c.right = {uint8_t(rnd() % 256), uint8_t(rnd() % 256), uint8_t(rnd() % 256)};
        c.weight = (rnd() % 101) / 100.0;
        RenderInfo info;
        auto out = render_strip(lamps, x, &info, &c, (rnd() % 1000) * 0.7);
        assert(static_cast<int>(out.size()) == x.led_count);
        assert(estimate_current_ma(out, x) <= x.current_budget_ma + 1e-9 ||
               x.current_budget_ma < x.idle_ma_per_led * x.led_count);
        ++checked;
    }
    // settings: JSON round trip, limits, old files get the tv layout
    {
        Settings r;
        r.strip_layout = kLayoutInset;
        r.edge_leds = 5;
        r.edge_strength = 0.8;
        r.edge_source = kEdgeJointSpace;
        cJSON* j = settings::to_json(r);
        Settings back;
        settings::merge_json(back, j);
        cJSON_Delete(j);
        assert(back.strip_layout == kLayoutInset && back.edge_leds == 5 && back.edge_strength == 0.8 &&
               back.edge_source == kEdgeJointSpace);
        cJSON* bad = cJSON_Parse("{\"strip_layout\":\"diagonal\",\"edge_leds\":99,\"edge_strength\":2,"
                                 "\"edge_source\":\"moon\"}");
        Settings v;
        settings::merge_json(v, bad);
        cJSON_Delete(bad);
        assert(v.strip_layout == kLayoutTv && v.edge_leds == 10 && v.edge_strength == 1.0 && v.edge_source == kEdgeAuto);
        cJSON* old = cJSON_Parse("{\"lamp_inset\":0.25,\"end_lamps\":true}");  // a v0.29 settings.json
        Settings o;
        settings::merge_json(o, old);
        cJSON_Delete(old);
        assert(o.strip_layout == kLayoutTv && o.end_lamps);
        cJSON* neg = cJSON_Parse("{\"edge_leds\":-4}");
        settings::merge_json(o, neg);
        cJSON_Delete(neg);
        assert(o.edge_leds == 0);
    }
    printf("[ok] v0.30: tv layout (lamps 2..4 end to end, edge colour from lamps 1/5 or JointSpace), ring peaks "
           "(max gap <= spacing + 1 at 3001 phases, 43/43 LEDs; inset layout had a %.0f-LED gap), %d budget cases\n",
           [] {
               Settings old = legacy_layout();
               old.end_lamps = true;
               double w = 0;
               for (int k = 0; k <= 3000; ++k) {
                   std::vector<int> p;
                   peak_weights(43, 5, old, &p, peak_drift_offset(old, old.peak_drift_period_s * k / 3000.0, 5));
                   w = std::max(w, max_peak_gap(p, 43));
               }
               return w;
           }(),
           checked);
}

static void test_discovery() {
    using discovery::answer_variants;
    const std::string udn = "2f402f80-da50-11e1-9b23-001788a45e01";
    // Switch off = every search gets all three answers (as before).
    assert((answer_variants("urn:dial-multiscreen-org:service:dial:1", udn, false) == std::vector<int>{0, 1, 2}));
    // Switch on: Hue-shaped searches only.
    assert((answer_variants("ssdp:all", udn, true) == std::vector<int>{0, 1, 2}));
    assert((answer_variants(" UPnP:RootDevice ", udn, true) == std::vector<int>{0}));
    assert((answer_variants("uuid:" + udn, udn, true) == std::vector<int>{1}));
    assert((answer_variants("urn:schemas-upnp-org:device:basic:1", udn, true) == std::vector<int>{2}));
    assert(answer_variants("urn:dial-multiscreen-org:service:dial:1", udn, true).empty());
    assert(answer_variants("urn:schemas-upnp-org:device:MediaServer:1", udn, true).empty());
    assert(answer_variants("-", udn, true).empty());

    char tmpl[] = "/tmp/ambi_disc_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    {
        discovery::Registry r(dir);
        assert(!r.hue_only());  // default off, no file yet
        assert(r.set_hue_only(true, nullptr));
        r.note("192.0.2.50", "ssdp", "urn:dial-multiscreen-org:service:dial:1", "Chrome/146", false, 1000);
        r.note("192.0.2.50", "ssdp", "urn:dial-multiscreen-org:service:dial:1", "Chrome/146", false, 2000);
        r.note("192.0.2.10", "description", "-", "TV", true, 1500);
        auto v = r.snapshot();
        assert(v.size() == 2);
        assert(v[0].ip == "192.0.2.50" && v[0].count == 2 && v[0].first_ms == 1000 && v[0].last_ms == 2000);
        assert(!v[0].answered && v[1].kind == "description");
        for (int i = 0; i < 100; ++i) r.note("10.0.0." + std::to_string(i), "ssdp", "ssdp:all", "x", true, 3000 + i);
        v = r.snapshot();
        assert(v.size() == discovery::Registry::kMaxEntries && v[0].ip == "10.0.0.99");
    }
    {
        discovery::Registry r(dir);  // the switch survives a restart, the list does not
        assert(r.hue_only() && r.snapshot().empty());
        assert(r.set_hue_only(false, nullptr));
    }
    assert(!discovery::Registry(dir).hue_only());
    std::string cmd = "rm -rf " + dir;
    assert(std::system(cmd.c_str()) == 0);
    printf("[ok] discovery: SSDP answered only for Hue searches when on, registry grouped/capped, switch persisted\n");
}

int main() {
    test_huestream_v1_xy();
    test_huestream_v2_rgb();
    test_huestream_rejects_garbage();
    test_xy_bri_to_rgb_matches_python_reference();
    test_dedup_tolerance();
    test_render_strip_fits_budget_under_pressure();
    test_render_strip_full_resolution_when_dim();
    test_srgb_to_led_linear();
    test_budget_never_exceeded_fuzz();
    test_pe0_identical_to_before();
    test_small_strip_budget_regression();
    test_uniform_white();
    test_peaks_keep_full_when_possible();
    test_channel_current_model();
    test_budget_ceiling();
    test_rise_from_shown();
    test_fit_budget();
    test_jointspace_curve();
    test_wear_aware();
    test_rise_limit();
    test_lamp_inset();
    test_v030_layout();
    test_blend_and_corners();
    test_blend_softness();
    test_rolling_peaks();
    test_v024_pipeline();
    test_strip_stats();
    test_strip_stats_v041();
    test_discovery();
    test_settings_roundtrip_and_corrupt_file();
    test_settings_merge_and_sanitize();
    test_pair_request_state();
    test_identity_store();
    test_hex_roundtrip();
    test_pairing_store_pair_and_persist();
    test_bridge_identity_derivation();
    test_black_and_zero_y_rules();
    test_color_trace_line();
    printf("\nAll core tests passed.\n");
    return 0;
}
