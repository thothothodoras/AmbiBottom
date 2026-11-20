#include "color_pipeline.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace color_pipeline {

// v0.32: "custom" behaves like "tv" wherever the custom positions do not
// apply (wrong length for this stream, or nothing to show).
static bool layout_is_tv(const Settings& s) { return s.strip_layout != kLayoutInset; }

bool custom_layout_active(const Settings& s, int lamp_count) {
    if (s.strip_layout != kLayoutCustom || lamp_count <= 0) return false;
    if (static_cast<int>(s.lamp_positions.size()) != lamp_count) return false;
    for (double p : s.lamp_positions)
        if (p >= 0.0) return true;
    return false;
}


RGB8 xy_bri_to_rgb(double x, double y, double bri_0_254) {
    // Verbatim port of fakebridge.py xy_bri_to_rgb (Wide Gamut RGB D65).
    if (y <= 0.0) {
        return {0, 0, 0};
    }
    double Y = std::max(0.0, std::min(1.0, bri_0_254 / 254.0));
    double X = (Y / y) * x;
    double Z = (Y / y) * (1.0 - x - y);
    double r = X * 1.656492 - Y * 0.354851 - Z * 0.255038;
    double g = -X * 0.707196 + Y * 1.655397 + Z * 0.036152;
    double b = X * 0.051713 - Y * 0.121364 + Z * 1.011530;

    auto gamma = [](double c) {
        c = std::max(0.0, c);
        return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    };
    r = gamma(r);
    g = gamma(g);
    b = gamma(b);
    double m = std::max({r, g, b, 1.0});
    auto to8 = [&](double c) {
        double v = std::round(255.0 * c / m);
        return static_cast<uint8_t>(std::max(0.0, std::min(255.0, v)));
    };
    return {to8(r), to8(g), to8(b)};
}

uint8_t srgb8_to_linear8(uint8_t v) {
    static const auto lut = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; ++i) {
            double c = i / 255.0;
            double lin = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            t[i] = static_cast<uint8_t>(std::lround(255.0 * lin));
        }
        return t;
    }();
    return lut[v];
}

bool xy_bri_changed(LampDedupState& prev, double x, double y, double bri_0_254,
                     double cie_tol, double bri_tol) {
    bool changed = !prev.has_value ||
                   std::fabs(x - prev.x) >= cie_tol ||
                   std::fabs(y - prev.y) >= cie_tol ||
                   std::fabs(bri_0_254 - prev.bri) >= bri_tol;
    if (changed) {
        prev.has_value = true;
        prev.x = x;
        prev.y = y;
        prev.bri = bri_0_254;
    }
    return changed;
}

bool rgb_changed(LampDedupState& prev, uint8_t r, uint8_t g, uint8_t b, int tol) {
    bool changed = !prev.has_value ||
                   std::abs(int(r) - int(prev.r)) >= tol ||
                   std::abs(int(g) - int(prev.g)) >= tol ||
                   std::abs(int(b) - int(prev.b)) >= tol;
    if (changed) {
        prev.has_value = true;
        prev.r = r;
        prev.g = g;
        prev.b = b;
    }
    return changed;
}

namespace {

struct RGBf {
    double r = 0, g = 0, b = 0;
};

RGBf to_f(const RGB8& c) { return {double(c.r), double(c.g), double(c.b)}; }

RGB8 to_8(const RGBf& c) {
    auto clamp8 = [](double v) {
        return static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(v))));
    };
    return {clamp8(c.r), clamp8(c.g), clamp8(c.b)};
}

// Piecewise-linear resample of `anchors` (evenly spaced across [0,1]) into
// exactly `count` evenly spaced output samples. Endpoints are reproduced
// exactly; count==1 returns the midpoint anchor.
std::vector<RGBf> resample(const std::vector<RGBf>& anchors, int count) {
    std::vector<RGBf> out(std::max(count, 0));
    const int k = static_cast<int>(anchors.size());
    if (k == 0 || count <= 0) {
        return out;
    }
    if (k == 1) {
        std::fill(out.begin(), out.end(), anchors[0]);
        return out;
    }
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

// v0.16: like resample(), but the k lamps sit at inset .. 1-inset of the
// strip instead of 0 .. 1; outside them the outer lamp's colour holds.
// inset = 0 gives exactly resample() (same arithmetic).
static std::vector<RGBf> lamp_curve(const std::vector<RGBf>& lamps, int count, double inset) {
    inset = std::max(0.0, std::min(0.45, inset));
    if (inset == 0.0) return resample(lamps, count);
    std::vector<RGBf> out(std::max(count, 0));
    const int k = static_cast<int>(lamps.size());
    if (k == 0 || count <= 0) return out;
    if (k == 1) {
        std::fill(out.begin(), out.end(), lamps[0]);
        return out;
    }
    for (int i = 0; i < count; ++i) {
        double x = (count == 1) ? 0.5 : double(i) / double(count - 1);
        double u = std::max(0.0, std::min(1.0, (x - inset) / (1.0 - 2.0 * inset)));
        double scaled = u * (k - 1);
        int seg = std::max(0, std::min(k - 2, static_cast<int>(std::floor(scaled))));
        double t = scaled - seg;
        const RGBf& a = lamps[seg];
        const RGBf& b = lamps[seg + 1];
        out[i] = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    }
    return out;
}

// The colour of `count` evenly spaced samples along anchors (position 0..1,
// colour) for blend_mode / blend_softness. Moved out of strip_curve() in
// v0.30 unchanged, so the tv layout uses the very same blending.
static std::vector<RGBf> blend_anchors(const std::vector<std::pair<double, RGBf>>& anchors, int count,
                                       const Settings& s) {
    std::vector<RGBf> out(std::max(count, 0));
    if (anchors.empty()) return out;
    for (int i = 0; i < count; ++i) {
        double x = (count == 1) ? 0.5 : double(i) / double(count - 1);
        if (s.blend_mode == kBlendSmooth) {
            size_t a = 0;
            while (a + 2 < anchors.size() && x > anchors[a + 1].first) ++a;
            const auto& [p, ca] = anchors[a];
            const auto& [q, cb] = anchors[std::min(a + 1, anchors.size() - 1)];
            double t = q > p ? std::max(0.0, std::min(1.0, (x - p) / (q - p))) : 0.0;
            out[i] = {ca.r + (cb.r - ca.r) * t, ca.g + (cb.g - ca.g) * t, ca.b + (cb.b - ca.b) * t};
        } else if (s.blend_softness > 0.0 && anchors.size() > 1) {
            // v0.23: zones with soft borders. Segment [p, q] holding x; the
            // border is halfway, the ramp is softness x (q - p) wide.
            size_t a = 0;
            while (a + 2 < anchors.size() && x > anchors[a + 1].first) ++a;
            const auto& [p, ca] = anchors[a];
            const auto& [q, cb] = anchors[a + 1];
            const double soft = std::min(1.0, s.blend_softness);
            const double m = 0.5 * (p + q), h = 0.5 * soft * (q - p);
            double t = 0.0;
            if (q <= p) t = x > p ? 1.0 : 0.0;
            else if (x >= m + h) t = 1.0;
            else if (x > m - h) t = h > 0.0 ? (x - (m - h)) / (2.0 * h) : (x > m ? 1.0 : 0.0);
            if (x < p) t = 0.0;
            out[i] = {ca.r + (cb.r - ca.r) * t, ca.g + (cb.g - ca.g) * t, ca.b + (cb.b - ca.b) * t};
        } else {
            // Zones: the nearest anchor (ties go left).
            size_t best = 0;
            double bd = 1e9;
            for (size_t a = 0; a < anchors.size(); ++a) {
                double d = std::fabs(x - anchors[a].first);
                if (d < bd - 1e-12) {
                    bd = d;
                    best = a;
                }
            }
            out[i] = anchors[best].second;
        }
    }
    return out;
}

// v0.30 tv layout: the bottom lamps from strip end to strip end, blended
// with blend_mode / blend_softness; the edge colour is added per LED later
// (apply_edges), so it does not depend on the anchor count.
static std::vector<RGBf> tv_curve(const std::vector<RGBf>& lamps, int count, const Settings& s) {
    const int k = static_cast<int>(lamps.size());
    if (k == 0 || count <= 0) return std::vector<RGBf>(std::max(count, 0));
    const std::vector<int> idx = bottom_lamp_indices(s, k);
    const std::vector<double> pos = strip_lamp_positions(s, k);
    auto mix = [](const RGBf& a, const RGBf& b) { return RGBf{(a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2}; };
    std::vector<std::pair<double, RGBf>> anchors;
    for (size_t j = 0; j < idx.size(); ++j) {
        if (j > 0 && s.blend_mode == kBlendZonesMid)
            anchors.push_back({(pos[j - 1] + pos[j]) / 2, mix(lamps[idx[j - 1]], lamps[idx[j]])});
        anchors.push_back({pos[j], lamps[idx[j]]});
    }
    return blend_anchors(anchors, count, s);
}

// v0.30 tv layout: the outermost edge_leds LEDs of each end take a share of
// the edge colour: edge_strength at the end LED, falling linearly (LED i of
// E: strength * (E - i) / E). JointSpace edges are scaled by the corner
// weight (it fades when JointSpace goes stale).
static void apply_edges(std::vector<RGBf>& leds, const std::vector<RGBf>& lamps, const Settings& s,
                        const Corners* corners) {
    const int n = static_cast<int>(leds.size());
    const int e = std::min(s.edge_leds, n / 2);
    if (e <= 0 || s.edge_strength <= 0.0) return;
    const int src = edge_source_resolved(s, static_cast<int>(lamps.size()));
    RGBf left, right;
    bool has_l = false, has_r = false;
    double share = std::min(1.0, s.edge_strength);
    if (src == kEdgeLamps) {
        left = lamps.front();
        right = lamps.back();
        has_l = has_r = true;
    } else if (src == kEdgeJointSpace && corners && corners->weight > 0.0) {
        has_l = corners->has_left;
        has_r = corners->has_right;
        left = to_f(corners->left);
        right = to_f(corners->right);
        share *= std::min(1.0, corners->weight);
    }
    auto blend = [](RGBf& c, const RGBf& t, double w) {
        c = {c.r + (t.r - c.r) * w, c.g + (t.g - c.g) * w, c.b + (t.b - c.b) * w};
    };
    for (int i = 0; i < e; ++i) {
        const double w = share * double(e - i) / double(e);
        if (has_l) blend(leds[i], left, w);
        if (has_r) blend(leds[n - 1 - i], right, w);
    }
}

// v0.18: the colour along the strip for blend_mode / corner_mode. The plain
// case (smooth, no corner colour) is exactly lamp_curve(), so v0.16
// output (and the pe=0 byte-identity test) is unchanged.
// v0.32 custom layout: every shown lamp at the position the user gave it
// (stream order -> position), blended with blend_mode / blend_softness.
static std::vector<RGBf> custom_curve(const std::vector<RGBf>& lamps, int count, const Settings& s) {
    std::vector<std::pair<double, RGBf>> shown;
    for (size_t j = 0; j < lamps.size(); ++j)
        if (s.lamp_positions[j] >= 0.0) shown.push_back({std::min(1.0, s.lamp_positions[j]), lamps[j]});
    std::stable_sort(shown.begin(), shown.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto mix = [](const RGBf& a, const RGBf& b) { return RGBf{(a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2}; };
    std::vector<std::pair<double, RGBf>> anchors;
    for (size_t j = 0; j < shown.size(); ++j) {
        if (j > 0 && s.blend_mode == kBlendZonesMid)
            anchors.push_back({(shown[j - 1].first + shown[j].first) / 2, mix(shown[j - 1].second, shown[j].second)});
        anchors.push_back(shown[j]);
    }
    return blend_anchors(anchors, count, s);
}

static std::vector<RGBf> strip_curve(const std::vector<RGBf>& lamps, int count, const Settings& s,
                                     const Corners* corners) {
    if (custom_layout_active(s, static_cast<int>(lamps.size()))) return custom_curve(lamps, count, s);
    if (layout_is_tv(s)) return tv_curve(lamps, count, s);
    const double inset = std::max(0.0, std::min(0.45, s.lamp_inset));
    const int k = static_cast<int>(lamps.size());
    // v0.24: with end_lamps the outer lamps ARE the ends: no corner anchors.
    const bool ends = end_lamps_active(s, k);
    const bool side = !ends && s.corner_mode == kCornerSide && corners && inset > 0.0 &&
                      (corners->has_left || corners->has_right) && corners->weight > 0.0;
    if (s.blend_mode == kBlendSmooth && !side && !ends) return lamp_curve(lamps, count, inset);
    std::vector<RGBf> out(std::max(count, 0));
    if (k == 0 || count <= 0) return out;

    auto mix = [](const RGBf& a, const RGBf& b) { return RGBf{(a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2}; };
    // v0.24: the side colour fading in/out (weight < 1) - between the lamp
    // and the half/half mix. weight 1 takes the exact v0.18 mix.
    auto corner_mix = [&](const RGBf& side_c, const RGBf& lamp) {
        RGBf m = mix(side_c, lamp);
        const double w = corners ? corners->weight : 1.0;
        if (w >= 1.0) return m;
        return RGBf{lamp.r + (m.r - lamp.r) * w, lamp.g + (m.g - lamp.g) * w, lamp.b + (m.b - lamp.b) * w};
    };
    std::vector<double> pos = strip_lamp_positions(s, k);
    std::vector<std::pair<double, RGBf>> anchors;
    if (inset > 0.0 && !ends) {
        RGBf c = lamps.front();
        if (side && corners->has_left) c = corner_mix(to_f(corners->left), lamps.front());
        anchors.push_back({0.0, c});
    }
    for (int j = 0; j < k; ++j) {
        if (j > 0 && s.blend_mode == kBlendZonesMid)
            anchors.push_back({(pos[j - 1] + pos[j]) / 2, mix(lamps[j - 1], lamps[j])});
        anchors.push_back({pos[j], lamps[j]});
    }
    if (inset > 0.0 && !ends) {
        RGBf c = lamps.back();
        if (side && corners->has_right) c = corner_mix(to_f(corners->right), lamps.back());
        anchors.push_back({1.0, c});
    }
    return blend_anchors(anchors, count, s);
}

std::vector<RGB8> render_at(const std::vector<RGB8>& lamps, int anchors, int led_count, const Settings& s,
                            const Corners* corners) {
    std::vector<RGBf> lamp_f;
    lamp_f.reserve(lamps.size());
    for (const auto& c : lamps) lamp_f.push_back(to_f(c));

    std::vector<RGBf> anchor_f = strip_curve(lamp_f, std::max(anchors, 1), s, corners);
    std::vector<RGBf> led_f = resample(anchor_f, led_count);
    if (layout_is_tv(s) && !custom_layout_active(s, static_cast<int>(lamps.size()))) apply_edges(led_f, lamp_f, s, corners);
    {
        const double b = std::max(0.0, std::min(1.0, s.brightness));
        const double wr = std::max(0.0, std::min(1.0, s.wb_r)), wg = std::max(0.0, std::min(1.0, s.wb_g)),
                     wbb = std::max(0.0, std::min(1.0, s.wb_b));
        if (b < 1.0 || wr < 1.0 || wg < 1.0 || wbb < 1.0)
            for (auto& c : led_f) {
                double t = 0.0;  // v0.49: 0 = full white balance, 1 = none (a pure colour)
                if (s.wb_keep_pure) {
                    const double mx = std::max(c.r, std::max(c.g, c.b)), mn = std::min(c.r, std::min(c.g, c.b));
                    const double sat = mx > 0.0 ? (mx - mn) / mx : 0.0;
                    t = std::max(0.0, std::min(1.0, (sat - 0.6) / 0.35));
                    t = t * t * (3.0 - 2.0 * t);
                }
                c = {c.r * b * (wr + (1.0 - wr) * t), c.g * b * (wg + (1.0 - wg) * t), c.b * b * (wbb + (1.0 - wbb) * t)};
            }
    }

    std::vector<RGB8> out(led_f.size());
    for (size_t i = 0; i < led_f.size(); ++i) out[i] = to_8(led_f[i]);
    return out;
}

}  // namespace

std::vector<double> lamp_positions(int lamp_count, double inset) {
    inset = std::max(0.0, std::min(0.45, inset));
    std::vector<double> pos;
    if (lamp_count <= 1) {
        pos.push_back(0.5);
    } else {
        for (int j = 0; j < lamp_count; ++j) pos.push_back(inset + (1.0 - 2.0 * inset) * double(j) / (lamp_count - 1));
    }
    return pos;
}


double led_colour_ma(const RGB8& c, const Settings& settings) {
    // With the scales at 1.0 every product is exact, so this is bit for bit
    // the v0.32 formula (r + g + b) / (3 * 255) * ma_per_led_full_white.
    double frac = (double(c.r) * settings.ma_scale_r + double(c.g) * settings.ma_scale_g +
                   double(c.b) * settings.ma_scale_b) / (3.0 * 255.0);
    return frac * settings.ma_per_led_full_white;
}

double estimate_current_ma(const std::vector<RGB8>& leds, const Settings& settings) {
    double total = settings.idle_ma_per_led * leds.size();
    for (const auto& c : leds) total += led_colour_ma(c, settings);
    return total;
}

static std::vector<RGB8> render_strip_anchors(const std::vector<RGB8>& lamps, const Settings& settings,
                                              int* used_anchors_out, RenderInfo* info, const Corners* corners) {
    const int led_count = std::max(settings.led_count, 1);
    const double budget = effective_budget_ma(settings);  // v0.33: never above the build-time ceiling
    if (lamps.empty()) {
        if (used_anchors_out) *used_anchors_out = 0;
        return std::vector<RGB8>(led_count, RGB8{0, 0, 0});
    }

    int hi = led_count;             // full resolution: as many anchors as LEDs
    int lo = std::max(settings.min_anchors, 1);
    lo = std::min(lo, hi);

    // Full-resolution frame first: if it already fits the budget, done -
    // this is the common case (most content is nowhere near full white).
    std::vector<RGB8> best = render_at(lamps, hi, led_count, settings, corners);
    if (info) info->wanted_ma = estimate_current_ma(best, settings);
    if (estimate_current_ma(best, settings) <= budget) {
        if (used_anchors_out) *used_anchors_out = hi;
        return best;
    }
    if (info) info->limit_mode = "anchors";

    // Binary search the largest anchor count that still fits the budget.
    // Monotonicity is a heuristic (coarser gradients tend to draw no more
    // current than finer ones for smooth content), bounded to ~6 iterations
    // on a 43-LED strip - cheap enough for a microcontroller.
    int best_m = lo;
    std::vector<RGB8> best_frame = render_at(lamps, lo, led_count, settings, corners);
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        std::vector<RGB8> frame = render_at(lamps, mid, led_count, settings, corners);
        if (estimate_current_ma(frame, settings) <= budget) {
            best_m = mid;
            best_frame = frame;
            lo = mid + 1;   // try for more resolution
        } else {
            hi = mid - 1;   // too much current, coarsen further
        }
    }
    // Safety net: anchor-coarsening cannot help uniform/near-uniform bright
    // content (e.g. all lamps at full white) - interpolating a flat signal
    // stays flat regardless of anchor count. If even min_anchors still
    // exceeds the budget, fall back to a global proportional scale-down of
    // the colour contribution only (never scale the fixed idle draw, and
    // never increase it). This is the hard safety guarantee: render_strip's
    // output must never exceed current_budget_ma, whatever the content.
    double current = estimate_current_ma(best_frame, settings);
    if (current > budget) {
        double idle = settings.idle_ma_per_led * best_frame.size();
        double color_current = current - idle;
        double available_for_color = budget - idle;
        double scale = 1.0;
        if (color_current > 0.0) {
            scale = std::max(0.0, available_for_color / color_current);
            scale = std::min(scale, 1.0);
        }
        if (info) {
            info->limit_mode = "uniform";
            info->min_scale = info->peak_scale = scale;
        }
        // floor(), not round(): the budget is a hard ceiling, so rounding
        // must never push the result back over it.
        for (auto& c : best_frame) {
            c.r = static_cast<uint8_t>(std::floor(c.r * scale));
            c.g = static_cast<uint8_t>(std::floor(c.g * scale));
            c.b = static_cast<uint8_t>(std::floor(c.b * scale));
        }
    }

    if (used_anchors_out) *used_anchors_out = best_m;
    return best_frame;
}


bool end_lamps_active(const Settings& s, int lamp_count) {
    return s.strip_layout == kLayoutInset && s.end_lamps && lamp_count == 5 && s.lamp_inset > 0.0;
}

std::vector<int> bottom_lamp_indices(const Settings& s, int lamp_count) {
    std::vector<int> idx;
    if (layout_is_tv(s) && lamp_count == 5) {
        idx = {1, 2, 3};  // angles 1 / 0 / 9; lamps 1 and 5 (angles 2 / 8) are the sides
    } else {
        for (int j = 0; j < lamp_count; ++j) idx.push_back(j);
    }
    return idx;
}

int edge_source_resolved(const Settings& s, int lamp_count) {
    if (s.strip_layout != kLayoutTv || s.edge_leds <= 0 || s.edge_strength <= 0.0) return kEdgeNone;
    if (s.edge_source == kEdgeJointSpace) return kEdgeJointSpace;
    if (lamp_count == 5) return kEdgeLamps;
    return s.edge_source == kEdgeLamps ? kEdgeNone : kEdgeJointSpace;
}

bool wants_side_colours(const Settings& s, int lamp_count) {
    if (layout_is_tv(s)) return edge_source_resolved(s, lamp_count) == kEdgeJointSpace;
    return s.corner_mode == kCornerSide;
}

std::vector<double> ring_peak_centres(int led_count, int n_peaks, double offset_leds) {
    std::vector<double> c;
    const int n = std::max(n_peaks, 1);
    const double len = std::max(led_count, 1);
    for (int j = 0; j < n; ++j) {
        double x = (j + 0.5) * len / n - 0.5 + offset_leds;
        x = std::fmod(x + 0.5, len);
        if (x < 0) x += len;
        c.push_back(x - 0.5);  // in [-0.5, len - 0.5)
    }
    std::sort(c.begin(), c.end());
    return c;
}

std::vector<double> strip_lamp_positions(const Settings& settings, int lamp_count) {
    if (custom_layout_active(settings, lamp_count)) {
        std::vector<double> pos;
        for (double p : settings.lamp_positions)
            if (p >= 0.0) pos.push_back(std::min(1.0, p));
        std::sort(pos.begin(), pos.end());
        return pos;
    }
    if (layout_is_tv(settings)) {
        const int nb = static_cast<int>(bottom_lamp_indices(settings, lamp_count).size());
        std::vector<double> pos;
        if (nb == 1) pos.push_back(0.5);
        for (int j = 0; nb > 1 && j < nb; ++j) pos.push_back(double(j) / (nb - 1));
        return pos;
    }
    if (!end_lamps_active(settings, lamp_count)) return lamp_positions(lamp_count, settings.lamp_inset);
    std::vector<double> mid = lamp_positions(3, settings.lamp_inset);
    return {0.0, mid[0], mid[1], mid[2], 1.0};
}

// Peak positions in [0,1] before any drift.
static std::vector<double> peak_base_positions(const Settings& settings, int lamp_count) {
    std::vector<double> pos;
    if (settings.peak_points > 0) {
        for (int j = 0; j < settings.peak_points; ++j) pos.push_back((j + 0.5) / settings.peak_points);
    } else if (end_lamps_active(settings, lamp_count)) {
        pos = lamp_positions(3, settings.lamp_inset);  // v0.24: lamps 2..4 only
    } else {
        pos = lamp_positions(lamp_count, settings.lamp_inset);
    }
    return pos;
}

// v0.30 tv layout: how many peaks (peak_points, or one per bottom lamp).
static int tv_peak_count(const Settings& settings, int lamp_count) {
    if (settings.peak_points > 0) return settings.peak_points;
    return std::max(1, static_cast<int>(bottom_lamp_indices(settings, lamp_count).size()));
}

double peak_drift_amplitude(const Settings& settings, int lamp_count) {
    if (settings.peak_drift_leds >= 0.0) return settings.peak_drift_leds;
    if (layout_is_tv(settings))  // half the ring spacing: back and forth covers every LED
        return 0.5 * std::max(settings.led_count, 1) / tv_peak_count(settings, lamp_count);
    const int n = std::max(settings.led_count, 1);
    std::vector<double> pos = peak_base_positions(settings, lamp_count);
    double spacing = pos.size() > 1 ? pos[1] - pos[0] : 0.25;
    return spacing * (n - 1);
}

double peak_drift_offset(const Settings& settings, double time_s, int lamp_count) {
    const double amp = peak_drift_amplitude(settings, lamp_count);
    if (amp <= 0.0 || settings.peak_drift_period_s <= 0.0 || time_s == 0.0) return 0.0;
    if (settings.peak_drift_shape == kDriftTriangle) {
        double ph = time_s / settings.peak_drift_period_s;
        ph -= std::floor(ph);
        // 0 -> +1 -> 0 -> -1 -> 0, constant speed (a sine lingers at the ends)
        double f = ph < 0.25 ? 4.0 * ph : ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0;
        return amp * f;
    }
    // the v0.23 formula exactly (sine with a fixed amplitude is byte-identical)
    const double two_pi = 6.28318530717958647692;
    return amp * std::sin(two_pi * time_s / settings.peak_drift_period_s);
}

LossOutput stream_loss_output(const Settings& s, double gap_ms) {
    LossOutput o;
    if (gap_ms <= s.loss_hold_ms) return o;
    const double t = gap_ms - s.loss_hold_ms;
    if (s.loss_fade_ms > 0.0 && t < s.loss_fade_ms) {
        // (1-x)^2: a linear PWM fade looks like "bright, bright, gone".
        const double x = t / s.loss_fade_ms;
        o.factor = (1.0 - x) * (1.0 - x);
        return o;
    }
    o.factor = 0.0;
    o.send = t - std::max(0.0, s.loss_fade_ms) < s.loss_black_s * 1000.0;
    return o;
}

std::vector<RGB8> scale_frame(const std::vector<RGB8>& leds, double factor) {
    factor = std::max(0.0, std::min(1.0, factor));
    std::vector<RGB8> out(leds.size());
    for (size_t i = 0; i < leds.size(); ++i)
        out[i] = {static_cast<uint8_t>(std::floor(leds[i].r * factor)), static_cast<uint8_t>(std::floor(leds[i].g * factor)),
                  static_cast<uint8_t>(std::floor(leds[i].b * factor))};
    return out;
}

std::vector<RGB8> fit_budget(const std::vector<RGB8>& leds, const Settings& settings) {
    const double budget = effective_budget_ma(settings);
    const double current = estimate_current_ma(leds, settings);
    if (current <= budget) return leds;
    const double idle = settings.idle_ma_per_led * leds.size();
    const double colour = current - idle;
    // floor() in scale_frame: the result can only be at or below budget
    return scale_frame(leds, colour > 0.0 ? std::max(0.0, (budget - idle) / colour) : 0.0);
}

std::vector<double> peak_weights(int led_count, int lamp_count, const Settings& settings,
                                  std::vector<int>* peak_leds, double offset_leds) {
    const double pe = std::max(0.0, std::min(1.0, settings.peak_emphasis));
    if (layout_is_tv(settings)) {
        // v0.30: evenly spaced on a ring of led_count LEDs, wrap-around drift:
        // at every moment the gap between neighbouring peaks is the same
        // (koryfes.png: with the inset layout all 3 could sit on one half).
        const int n = tv_peak_count(settings, lamp_count);
        const double len = std::max(led_count, 1);
        const std::vector<double> c = ring_peak_centres(led_count, n, offset_leds);
        const double half = 0.5 * len / n;
        std::vector<double> w(std::max(led_count, 0), 1.0);
        for (int i = 0; i < led_count; ++i) {
            double d = 1e9;
            for (double p : c) {
                double dd = std::fabs(double(i) - p);
                dd = std::fmod(dd, len);
                d = std::min(d, std::min(dd, len - dd));
            }
            double t = std::min(1.0, d / half);
            double bump = 0.5 * (1.0 - std::cos(3.14159265358979323846 * t));
            w[i] = 1.0 - 0.85 * pe * bump;
        }
        if (peak_leds) {
            peak_leds->clear();
            for (double p : c) {
                int idx = std::max(0, std::min(led_count - 1, static_cast<int>(std::floor(p + 0.5))));
                if (std::find(peak_leds->begin(), peak_leds->end(), idx) == peak_leds->end()) peak_leds->push_back(idx);
            }
            std::sort(peak_leds->begin(), peak_leds->end());
        }
        return w;
    }
    std::vector<double> pos = peak_base_positions(settings, lamp_count);  // peak positions in [0,1]
    if (offset_leds != 0.0 && led_count > 1) {
        const double dx = offset_leds / double(led_count - 1);
        for (double& p : pos) p = std::max(0.0, std::min(1.0, p + dx));
    }
    // Half the spacing between neighbouring peaks: where the weight bottoms
    // out. With a single peak, the far end of the strip.
    double half = 0.5;
    if (pos.size() > 1) half = 0.5 * (pos[1] - pos[0]);
    std::vector<double> w(std::max(led_count, 0), 1.0);
    if (peak_leds) peak_leds->clear();
    for (int i = 0; i < led_count; ++i) {
        double x = led_count == 1 ? 0.5 : double(i) / (led_count - 1);
        double d = 1e9;
        for (double p : pos) d = std::min(d, std::fabs(x - p));
        double t = std::min(1.0, d / (pos.size() > 1 ? half : std::max(pos[0], 1.0 - pos[0])));
        double bump = 0.5 * (1.0 - std::cos(3.14159265358979323846 * t));  // 0 at a peak, 1 halfway
        w[i] = 1.0 - 0.85 * pe * bump;
    }
    if (peak_leds) {
        for (double p : pos) {
            int idx = led_count == 1 ? 0 : static_cast<int>(std::lround(p * (led_count - 1)));
            if (peak_leds->empty() || peak_leds->back() != idx) peak_leds->push_back(idx);
        }
    }
    return w;
}

static std::vector<RGB8> apply_scales(const std::vector<RGB8>& frame, const std::vector<double>& w, double lambda,
                                      double* peak_min, double* any_min, const std::vector<int>& peaks) {
    std::vector<RGB8> out(frame.size());
    double mn = 1.0;
    for (size_t i = 0; i < frame.size(); ++i) {
        double sc = std::min(1.0, lambda * w[i]);
        mn = std::min(mn, sc);
        // floor(): the budget is a hard ceiling.
        out[i] = {static_cast<uint8_t>(std::floor(frame[i].r * sc)), static_cast<uint8_t>(std::floor(frame[i].g * sc)),
                  static_cast<uint8_t>(std::floor(frame[i].b * sc))};
    }
    if (any_min) *any_min = mn;
    if (peak_min) {
        double pm = 1.0;
        for (int p : peaks)
            if (p >= 0 && p < static_cast<int>(w.size())) pm = std::min(pm, std::min(1.0, lambda * w[p]));
        *peak_min = pm;
    }
    return out;
}

std::vector<double> wear_from_mah(const std::vector<double>& mah, int led_count) {
    const int n = std::max(led_count, 0);
    std::vector<double> out(n, 0.0);
    if (n == 0 || mah.empty()) return out;
    double lowest = 1e300;
    for (int i = 0; i < n && i < static_cast<int>(mah.size()); ++i)
        if (std::isfinite(mah[i])) lowest = std::min(lowest, std::max(0.0, mah[i]));
    if (lowest > 1e299) return out;
    std::vector<double> m(n, lowest);
    for (int i = 0; i < n && i < static_cast<int>(mah.size()); ++i)
        if (std::isfinite(mah[i])) m[i] = std::max(0.0, mah[i]);
    std::vector<double> sm(n, 0.0);
    double mean = 0.0, low = 1e300;
    for (int i = 0; i < n; ++i) {
        double sum = 0.0;
        for (int k = -2; k <= 2; ++k) sum += m[std::max(0, std::min(n - 1, i + k))];  // ends repeat
        sm[i] = sum / 5.0;
        mean += sm[i];
        low = std::min(low, sm[i]);
    }
    mean /= n;
    if (mean <= 0.0) return out;
    for (int i = 0; i < n; ++i) out[i] = std::max(0.0, std::min(1.0, (sm[i] - low) / (kWearFullSpread * mean)));
    return out;
}

std::vector<RGB8> render_strip(const std::vector<RGB8>& lamps, const Settings& settings, RenderInfo* info,
                                const Corners* corners, double time_s, const std::vector<double>* wear) {
    if (info) *info = RenderInfo{};
    if (settings.peak_emphasis <= 0.0 || lamps.empty()) {
        int used = 0;
        auto out = render_strip_anchors(lamps, settings, &used, info, corners);
        if (info) info->used_anchors = used;
        return out;
    }
    const int led_count = std::max(settings.led_count, 1);
    const double budget = effective_budget_ma(settings);  // v0.33: never above the build-time ceiling
    std::vector<RGB8> full = render_at(lamps, led_count, led_count, settings, corners);
    if (info) info->used_anchors = led_count;
    const double wanted = estimate_current_ma(full, settings);
    if (info) info->wanted_ma = wanted;
    if (wanted <= budget) return full;

    std::vector<int> peaks;
    const double drift = peak_drift_offset(settings, time_s, static_cast<int>(lamps.size()));
    std::vector<double> w = peak_weights(led_count, static_cast<int>(lamps.size()), settings, &peaks, drift);
    // lambda: 0 = black, >= 1/min(w) = untouched. Current grows with
    // lambda, so binary search the largest lambda that fits.
    double lo = 0.0, hi = 1.0 / 0.15 + 1e-9;
    // v0.33: wear-aware weights. Only here (over budget, pe > 0); with
    // wear_strength 0 or no wear data this block does not run at all.
    const double ws = std::max(0.0, std::min(1.0, settings.wear_strength));
    bool wear_applied = false;
    if (ws > 0.0 && wear && static_cast<int>(wear->size()) >= led_count) {
        double min_w = 1.0;
        for (int i = 0; i < led_count; ++i) {
            const double wi = std::max(0.0, std::min(1.0, (*wear)[i]));
            if (wi > 0.0) wear_applied = true;
            w[i] *= 1.0 - kWearMaxCut * ws * wi;
            min_w = std::min(min_w, w[i]);
        }
        hi = 1.0 / std::max(min_w, 1e-6) + 1e-9;  // the weights can be lower than 0.15 now
    }
    for (int it = 0; it < 40; ++it) {
        double mid = 0.5 * (lo + hi);
        auto f = apply_scales(full, w, mid, nullptr, nullptr, peaks);
        if (estimate_current_ma(f, settings) <= budget) lo = mid;
        else hi = mid;
    }
    double pm = 1.0, mn = 1.0;
    auto out = apply_scales(full, w, lo, &pm, &mn, peaks);
    // Hard guarantee (budget below the idle draw etc.): uniform floor-scale
    // until it fits, like the pe=0 safety net.
    if (estimate_current_ma(out, settings) > budget) {
        double idle = settings.idle_ma_per_led * out.size();
        double cur = estimate_current_ma(out, settings) - idle;
        double sc = cur > 0 ? std::max(0.0, std::min(1.0, (budget - idle) / cur)) : 0.0;
        for (auto& c : out) {
            c.r = static_cast<uint8_t>(std::floor(c.r * sc));
            c.g = static_cast<uint8_t>(std::floor(c.g * sc));
            c.b = static_cast<uint8_t>(std::floor(c.b * sc));
        }
        pm *= sc;
        mn *= sc;
    }
    if (info) {
        info->limit_mode = "peaks";
        info->peak_scale = pm;
        info->min_scale = mn;
        info->peak_leds = peaks;
        info->peak_offset_leds = drift;
        info->wear_applied = wear_applied;
    }
    return out;
}

std::vector<RGB8> render_strip(const std::vector<RGB8>& lamps, const Settings& settings, int* used_anchors_out) {
    RenderInfo info;
    auto out = render_strip(lamps, settings, &info);
    if (used_anchors_out) *used_anchors_out = info.used_anchors;
    return out;
}

std::vector<RGB8> limit_rise(const std::vector<RGB8>& prev, const std::vector<RGB8>& target, double dt_ms,
                             double rise_ms) {
    if (rise_ms <= 0.0 || prev.size() != target.size() || dt_ms < 0.0 || dt_ms >= 4.0 * rise_ms) return target;
    const double step = 255.0 * dt_ms / rise_ms;
    std::vector<RGB8> out(target.size());
    auto ch = [step](uint8_t p, uint8_t t) -> uint8_t {
        if (t <= p) return t;  // fall (or equal): immediate
        // ceil(): with a very long rise_ms the step can be < 1 per frame;
        // floor() would then never move. Overshoots the rate by < 1 unit.
        double v = std::min(double(t), std::ceil(p + step));
        return static_cast<uint8_t>(std::max(double(p), v));
    };
    for (size_t i = 0; i < target.size(); ++i)
        out[i] = {ch(prev[i].r, target[i].r), ch(prev[i].g, target[i].g), ch(prev[i].b, target[i].b)};
    return out;
}

std::vector<RGB8> rise_from_shown(RiseState& state, const std::vector<RGB8>& shown, const std::vector<RGB8>& target,
                                  double dt_ms, double rise_ms, double start_ms) {
    if (rise_ms <= 0.0 && start_ms <= 0.0) {
        state.remaining_ms = 0.0;
        return target;
    }
    const bool restart = shown.size() != target.size() || dt_ms < 0.0 || dt_ms > kRiseGapMs;
    if (!restart && state.remaining_ms <= 0.0) return limit_rise(shown, target, dt_ms, rise_ms);
    const double dt = restart ? kRiseGapStepMs : dt_ms;
    if (restart) state.remaining_ms = std::max(rise_ms, start_ms);  // v0.42: fade_in_ms
    std::vector<RGB8> prev(target.size());  // black where nothing was shown
    for (size_t i = 0; i < prev.size() && i < shown.size(); ++i) prev[i] = shown[i];
    std::vector<RGB8> out = limit_rise(prev, target, dt, rise_ms);
    const double frac = std::min(1.0, dt / state.remaining_ms);
    auto ch = [frac](uint8_t p, uint8_t t, uint8_t limited) -> uint8_t {
        if (t <= p) return limited;  // a fall: immediate, as limit_rise gave it
        const double cap = double(p) + std::ceil(double(t - p) * frac);  // ceil: always moves
        return static_cast<uint8_t>(std::min(double(limited), cap));
    };
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = {ch(prev[i].r, target[i].r, out[i].r), ch(prev[i].g, target[i].g, out[i].g),
                  ch(prev[i].b, target[i].b, out[i].b)};
    state.remaining_ms = std::max(0.0, state.remaining_ms - dt);
    return out;
}

std::vector<RGB8> smooth_small_changes(const std::vector<RGB8>& shown, const std::vector<RGB8>& next, double dt_ms,
                                       const Settings& s) {
    if (s.smooth_ms <= 0.0 || shown.size() != next.size() || dt_ms <= 0.0 || dt_ms > kRiseGapMs) return next;
    std::vector<RGB8> out(next.size());
    auto ch = [&](uint8_t p, uint8_t n) -> uint8_t {
        const double d = double(n) - double(p);
        const double k = std::max(0.0, 1.0 - std::fabs(d) / kSmoothBig);
        const double tau = s.smooth_ms * k;
        if (tau <= 1.0 || d == 0.0) return n;
        double v = p + d * (1.0 - std::exp(-dt_ms / tau));
        long r = std::lround(v);
        if (r == p) r = p + (d > 0 ? 1 : -1);  // always moves
        return static_cast<uint8_t>(std::max(0L, std::min(255L, r)));
    };
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = {ch(shown[i].r, next[i].r), ch(shown[i].g, next[i].g), ch(shown[i].b, next[i].b)};
    return fit_budget(out, s);
}

bool is_dark(const std::vector<RGB8>& leds) {
    for (const RGB8& c : leds)
        if (c.r > kDarkLevel || c.g > kDarkLevel || c.b > kDarkLevel) return false;
    return true;
}

std::vector<RGB8> fade_to_dark(DarkFadeState& state, const std::vector<RGB8>& shown, const std::vector<RGB8>& next,
                               double dt_ms, double fade_ms) {
    if (fade_ms <= 0.0 || !is_dark(next) || shown.size() != next.size()) {
        state.active = false;
        return next;
    }
    if (!state.active) {
        if (is_dark(shown)) return next;  // nothing to fade
        state.active = true;
        state.from = shown;
        state.elapsed_ms = 0.0;
    }
    state.elapsed_ms += std::max(0.0, std::min(dt_ms, 1000.0));
    const double x = state.elapsed_ms / fade_ms;
    if (x >= 1.0) {
        state.active = false;
        return next;
    }
    const double f = (1.0 - x) * (1.0 - x);
    std::vector<RGB8> out(next.size());
    auto ch = [f](uint8_t from, uint8_t n) {
        const double v = std::floor(from * f);
        return static_cast<uint8_t>(std::max(double(n), v));
    };
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = {ch(state.from[i].r, next[i].r), ch(state.from[i].g, next[i].g), ch(state.from[i].b, next[i].b)};
    return out;
}

RGB8 jointspace_to_led(uint8_t r, uint8_t g, uint8_t b, const Settings& s) {
    const double full = s.corner_js_full_scale > 0.0 ? s.corner_js_full_scale : 23.0;
    auto c = [&](uint8_t v) {
        // linear with full scale 23 = the v0.18..v0.32 expression in main.cpp, bit for bit
        const uint8_t level = static_cast<uint8_t>(std::lround(255.0 * std::min(1.0, v / full)));
        return s.corner_js_curve == kJsSrgb ? srgb8_to_linear8(level) : level;
    };
    return RGB8{c(r), c(g), c(b)};
}
const char* js_curve_name(int m) { return m == kJsSrgb ? "srgb" : "linear"; }
int js_curve_from(const std::string& s, int fallback) {
    if (s == "linear") return kJsLinear;
    if (s == "srgb") return kJsSrgb;
    return fallback;
}

const char* strip_layout_name(int m) { return m == kLayoutInset ? "inset" : m == kLayoutCustom ? "custom" : "tv"; }
int strip_layout_from(const std::string& s, int fallback) {
    if (s == "tv") return kLayoutTv;
    if (s == "inset") return kLayoutInset;
    if (s == "custom") return kLayoutCustom;
    return fallback;
}
const char* edge_source_name(int m) {
    return m == kEdgeLamps ? "lamps" : m == kEdgeJointSpace ? "jointspace" : m == kEdgeNone ? "none" : "auto";
}
int edge_source_from(const std::string& s, int fallback) {
    if (s == "auto") return kEdgeAuto;
    if (s == "lamps") return kEdgeLamps;
    if (s == "jointspace") return kEdgeJointSpace;
    return fallback;
}
const char* blend_mode_name(int m) {
    return m == kBlendZones ? "zones" : m == kBlendZonesMid ? "zones_mid" : "smooth";
}
const char* corner_mode_name(int m) { return m == kCornerLamp ? "lamp" : "side"; }
const char* drift_shape_name(int m) { return m == kDriftSine ? "sine" : "triangle"; }
int drift_shape_from(const std::string& s, int fallback) {
    if (s == "sine") return kDriftSine;
    if (s == "triangle") return kDriftTriangle;
    return fallback;
}
int blend_mode_from(const std::string& s, int fallback) {
    if (s == "smooth") return kBlendSmooth;
    if (s == "zones") return kBlendZones;
    if (s == "zones_mid") return kBlendZonesMid;
    return fallback;
}
int corner_mode_from(const std::string& s, int fallback) {
    if (s == "lamp") return kCornerLamp;
    if (s == "side") return kCornerSide;
    return fallback;
}

}  // namespace color_pipeline
