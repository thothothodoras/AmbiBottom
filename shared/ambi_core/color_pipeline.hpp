// Colour pipeline: Hue XY+bri/RGB -> strip RGB, with:
//  - simplified (non-calibrated) XY+bri -> sRGB conversion, ported verbatim
//    from the user's own fakebridge.py (already validated against the real TV)
//  - per-lamp tolerance dedup (skip re-render when nothing changed enough)
//  - two-stage piecewise-linear interpolation: N lamps -> M "anchors" -> LED_COUNT
//    physical LEDs. When the ideal (M = LED_COUNT) frame would exceed the
//    current budget, M shrinks (coarser gradient, same LED_COUNT lit, no dark
//    gaps) instead of globally dimming everything. See PROJECT notes:
//    "dynamic spatial resolution based on current headroom".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// v0.33: the highest current limit any setting can ask for, fixed when the
// program is built (cmake -DAMBI_MAX_BUDGET_MA=..., or -D AMBI_MAX_BUDGET_MA=...
// for the usermod). 900 = the largest USB port of the reference TV (its label,
// NOT measured). It is applied twice: settings::sanitize() clamps
// current_budget_ma to it, and the limiter itself never uses more
// (effective_budget_ma), so no settings path and no caller can go above it.
#ifndef AMBI_MAX_BUDGET_MA
#define AMBI_MAX_BUDGET_MA 900
#endif

namespace color_pipeline {

constexpr double kMaxBudgetMa = AMBI_MAX_BUDGET_MA;

struct RGB8 {
    uint8_t r = 0, g = 0, b = 0;
};

// Exact port of fakebridge.py's xy_bri_to_rgb(): Wide Gamut RGB D65 matrix,
// sRGB gamma, normalized so the brightest channel hits 255 (matches the
// Python reference bit-for-bit for the same inputs, verified by unit test).
// bri_0_254 follows the Hue convention (0..254, not 0..255).
RGB8 xy_bri_to_rgb(double x, double y, double bri_0_254);

// Direct 16-bit RGB channel -> 8-bit (Hue "RGB" colourspace, colorspace=0):
// matches fakebridge.py's `a >> 8` truncation.
inline uint8_t rgb16_to_8(uint16_t v) { return static_cast<uint8_t>(v >> 8); }

// v0.12: xy_bri_to_rgb() returns display-encoded sRGB (for a screen), but a
// WS2812 LED is linear: PWM value 128 emits ~50 % light, while sRGB 128
// means only ~22 %. Sending sRGB straight to the strip makes dark and
// middle colours far too bright and washed out, and makes the current
// estimate (linear in PWM values) far too high. This decodes sRGB 8-bit
// to linear 8-bit (the PWM value the LED needs); same formula as the
// monitor page's Color.srgbToLinear(). Exact at 0 and 255.
uint8_t srgb8_to_linear8(uint8_t v);
inline RGB8 srgb_to_led(RGB8 c) { return {srgb8_to_linear8(c.r), srgb8_to_linear8(c.g), srgb8_to_linear8(c.b)}; }

struct Settings {
    int led_count = 43;             // physical LEDs on the strip (runtime-configurable)
    bool diffuser = false;          // informational only for now; does not change the algorithm
    // v0.32: default 450 mA (was 800 since v0.14): the strip is fed from a TV
    // USB port behind a 1.1 A polyfuse. NOT a measured value - set it to what
    // your supply gives. A saved settings.json keeps its own value.
    double current_budget_ma = 450.0;
    double idle_ma_per_led = 1.0;       // WS2812 idle draw per LED
    // v0.34: 45 = MEASURED on the reference strip (BTF WS2812B ECO 30 LED/m,
    // 2026-11-17, USB meter: 5 LEDs 0.20 A, 10 LEDs 0.42 A full white -> ~44,
    // range 40-48 from the meter's 10 mA steps); was 60 = the vendor spec.
    // Another strip: measure it (README "Measuring mA per LED").
    double ma_per_led_full_white = 45.0;
    // v0.33: how much each colour channel draws, relative to one third of
    // ma_per_led_full_white (a WS2812B has three separate drivers; the model
    // until v0.32 gave them the same share). Current of one LED =
    //   idle + (r*ma_scale_r + g*ma_scale_g + b*ma_scale_b) / (3*255) * ma_per_led_full_white
    // 1 / 1 / 1 = exactly the v0.32 estimate (bit-identical). NOT measured:
    // measure red, green and blue alone with a USB meter (docs/HARDWARE.md)
    // and enter (channel mA) / (white mA / 3). Entering less than the real
    // value makes the limit let more current through than it says.
    double ma_scale_r = 1.0, ma_scale_g = 1.0, ma_scale_b = 1.0;
    int min_anchors = 2;                 // never collapse below this many distinct points
    // v0.13 (Phase 2): how the budget is enforced when the strip doesn't fit.
    // 0 = exactly the pre-v0.13 behaviour (fewer anchors, then a uniform
    // scale-down). >0 = full resolution, and the LEDs between the peaks dim
    // first (smooth raised-cosine weights) while the peaks stay at 100 % as
    // long as the budget allows. 1 = strongest. The budget stays a hard limit.
    double peak_emphasis = 0.5;
    // Where the peaks are: 0 = at the TV lamp positions (as render_strip
    // spreads them: lamp i of k at i/(k-1) along the strip); N > 0 = N
    // evenly spaced points at (j+0.5)/N.
    int peak_points = 0;
    // Max rise speed per LED channel: 0 -> 255 takes rise_ms. Falls are
    // immediate. 0 = off. Never raises a value above its target, so it can
    // never push the strip over the budget.
    double rise_ms = 120.0;
    // v0.42 (user: «δε σβήνει ήρεμα, σβήνει απότομα και ανοίγει»): the strip lights up over
    // fade_in_ms after a start / pause (was rise_ms = 0.12 s), and when the TV's picture goes
    // dark (every LED at most kDarkLevel: Ambilight off, the end of a programme) it fades out
    // over fade_out_ms instead of going black at once. Normal content still falls at once.
    // 0 = as before.
    double fade_in_ms = 800.0;
    double fade_out_ms = 1500.0;
    // v0.44 (user: «το μαμά Ambilight έχει πιο χαλαρές μεταβάσεις»; the user's video: at a scene cut
    // the factory sides and our strip changed in the same camera frame, but our strip wobbled up /
    // down a little where the factory light moved smoothly): small changes are smoothed with a time
    // constant of up to smooth_ms; the bigger the change, the shorter it gets, and a change of
    // kSmoothBig or more (a scene cut) passes at once. 0 = off (as before).
    double smooth_ms = 150.0;
    // v0.16: where the outer lamps sit on the strip, as a fraction of the
    // strip length from each end (0..0.45). 0 = at the very ends (before
    // v0.16), 0.25 = quarters: with 3 lamps at 25 % / 50 % / 75 % (the
    // user's choice: split the screen in 4, take the 3 points between).
    // LEDs outside the outer lamps hold that lamp's colour. The colour
    // interpolation and the peaks (peak_points = 0) both use these positions.
    // For reference, the group locations handed to the TV are x = -0.8/0/+0.8
    // (10 % / 50 % / 90 %), i.e. 0.1 here.
    double lamp_inset = 0.25;
    // v0.18: how the colours go from lamp to lamp (like the old monitor's
    // «Μετάβαση χρωμάτων», but on the real strip):
    //   kBlendSmooth   = linear blend between neighbouring lamps
    //   kBlendZones    = each LED takes the nearest lamp; hard switch halfway
    //   kBlendZonesMid = zones, plus one zone between each pair of lamps with
    //                    the average of the two (3 lamps -> 5 zones)
    int blend_mode = 0;
    // v0.18: the ends of the strip, beyond the outer lamps (needs lamp_inset > 0):
    //   kCornerLamp = hold the outer lamp's colour
    //   kCornerSide = the very end takes the average of the outer lamp and
    //                 the TV's side colour at that corner (JointSpace zone
    //                 touching the bottom), so the strip blends into the
    //                 sides; without live JointSpace -> like kCornerLamp
    int corner_mode = 1;
    // v0.23 (NEXT 9): «Απαλότητα ορίων» for blend_mode zones / zones_mid.
    // 0 = hard zone borders, exactly as v0.18 (byte-identical). s > 0: each
    // border becomes a linear ramp of s x (distance between the two zone
    // centres), centred on the border, so a zone stays flat in its middle.
    // 1 = the ramps meet = the same as kBlendSmooth. No effect on smooth.
    double blend_softness = 0.0;
    // v0.23 (NEXT 5): rolling peaks. While the limiter keeps peaks (pe > 0
    // and the frame is over budget), the peak positions drift together by
    // up to +-peak_drift_leds LEDs (a sine, one full cycle every
    // peak_drift_period_s seconds), so the same few LEDs are not always the
    // hardest-driven. 0 = fixed peaks (as v0.13-v0.22). Only the weights
    // move; the budget stays a hard limit and pe = 0 is unaffected.
    //
    // v0.24 (PLAN v0.24 item 3): peak_drift_leds < 0 = "auto" = the spacing
    // between two neighbouring peaks (10.5 LEDs with 43 LEDs, 3 peaks at
    // 25/50/75 %), so over one cycle the peaks pass over every LED. New
    // defaults: auto, triangle, 300 s (v0.23: 2 LEDs, sine, 60 s).
    double peak_drift_leds = -1.0;
    double peak_drift_period_s = 300.0;
    int peak_drift_shape = 1;  // kDriftSine | kDriftTriangle
    // v0.24 (PLAN v0.24 item 1): «Λάμπες στις άκρες». Only with 5 lamps and
    // lamp_inset > 0: lamp 1 -> the first LED, lamps 2..4 where the 3 lamps
    // of the 3-lamp layout sit (inset / 50 % / 1-inset), lamp 5 -> the last
    // LED. corner_mode is not used then (the TV's own lamps 1 and 5 are the
    // corners), and the limiter peaks are only at lamps 2..4. false = exactly
    // v0.23. Ignored with 3 lamps.
    bool end_lamps = false;
    // v0.24 (review item 1): when the Hue stream stops, the strip holds the
    // last frame for loss_hold_ms, fades to black over loss_fade_ms, then
    // keeps sending black for loss_black_s (so WLED stays in realtime mode
    // and does not jump to its own preset), then stops sending.
    double loss_hold_ms = 1000.0;
    double loss_fade_ms = 1000.0;
    double loss_black_s = 30.0;
    // v0.30: «Διάταξη ταινίας», from the 2026-11-06 measurements on the real
    // TV (tools/clock bars): the TV gives each Hue lamp the colour of the
    // place its Angle (0-9, set in the TV's Ambilight+hue wizard) points at,
    // and only 3 angles are on the bottom (1 bottom-left, 0 middle, 9
    // bottom-right; they read x = -0.82 / 0 / +0.82 of the screen width).
    // With the user's angles 2/1/0/9/8, lamps 2..4 are the bottom and lamps
    // 1/5 read the lower left/right SIDE.
    //   kLayoutTv    = the bottom lamps (2..4 of 5, or all of 3) from strip
    //                  end to strip end (0 / 50 % / 100 %), blend_mode and
    //                  blend_softness as before; the outermost edge_leds LEDs
    //                  get a light mix of the "edge colour" (edge_source).
    //                  Peaks: evenly spaced round the strip (as on a ring),
    //                  so they stay evenly spread while they drift.
    //                  lamp_inset / end_lamps / corner_mode are not used.
    //   kLayoutInset = exactly v0.16..v0.29 (lamp_inset, end_lamps, corner_mode).
    int strip_layout = 0;
    int edge_leds = 3;            // 0 = no edge colour; at most half the strip
    double edge_strength = 0.5;   // share of the edge colour at the outermost LED, falling linearly to 0
    int edge_source = 0;          // kEdgeAuto | kEdgeLamps | kEdgeJointSpace
    // v0.32: overall brightness, 0..1, applied to the colours BEFORE the
    // current limit (so the limit still holds and has less to cut). 1 = as before.
    double brightness = 1.0;
    // v0.48: white balance - per-channel gains 0.2..1 (applied with the brightness, before the current
    // limit). The WS2812's white is cold next to the TV's own LEDs (user's photo: a black picture with
    // white text, warm sides, bluish bottom): lowering blue (and a little green) warms it. 1/1/1 = off.
    double wb_r = 1.0, wb_g = 1.0, wb_b = 1.0;
    // v0.49: keep pure colours. The TV's Hue "white" is itself bluish (linear ~110/135/215, saturation
    // ~0.5), so the gains have to act there in full - but a pure blue / red / yellow (saturation ~1)
    // should not be dimmed by them. Per LED: s = (max - min) / max; the gains act fully below s 0.6 and
    // fade out (smoothstep) up to s 0.95. false = the gains everywhere (v0.48).
    bool wb_keep_pure = true;
    // v0.49 (user: «αν κάποιος έχει ένα USB θα πρέπει να υπολογίσουμε και το ρεύμα του ESP»): the board's
    // own current when it shares the strip's USB port; the strip may then use current_budget_ma minus
    // this. 0 = the board has its own port (the reference install). Set by the ESP's device settings;
    // not in the strip settings JSON.
    double esp_same_port_ma = 0.0;
    // v0.33: wear-aware limiter. Only while a frame is over the budget and
    // peak_emphasis > 0: the limiter's weight of each LED is multiplied by
    // 1 - kWearMaxCut * wear_strength * wear[i], where wear[i] (0..1) says how
    // much more that LED has been used than the least-used ones (see
    // wear_from_mah). So the most-used LEDs are dimmed first and the least-used
    // keep their brightness; the budget is untouched. 0 = off = bit-identical
    // to v0.32. The colours' positions never move, and a frame within the
    // budget is never changed. Whether it shows on the wall is NOT judged yet
    // (try 0.3). The wear data comes from the bridge's strip_stats (Linux
    // bridge only; the ESP32 usermod keeps no such log and passes none).
    double wear_strength = 0.0;
    // v0.33: how a JointSpace side value (the TV's own left/right zones, used
    // for the strip's corners / edges) becomes an LED level.
    //   corner_js_full_scale: the JointSpace value that means "full" (255).
    //     23 was MEASURED ONLY at Ambilight brightness 3/10 (2026-11-06); the
    //     TV scales these values with its brightness setting, so another
    //     setting needs another number (values above it are cut to full).
    //   corner_js_curve: kJsLinear = value / full_scale straight to the LED
    //     level (v0.18..v0.32, bit-identical); kJsSrgb = the same curve the Hue
    //     lamps go through (srgb8_to_linear8), so a middle side value gives
    //     the same LED level as a middle lamp value (linear: ~50 %, srgb:
    //     ~22 %). Which one matches the TV's own strips is NOT measured.
    int corner_js_curve = 0;
    double corner_js_full_scale = 23.0;
    // v0.32: strip_layout "custom": where each lamp of the stream sits on the
    // strip, one number per lamp in stream order, 0 = strip start .. 1 = strip
    // end; a negative number = that lamp is not shown. Used only when its
    // length equals the number of lamps in the stream and at least one is
    // >= 0; otherwise the layout falls back to "tv".
    std::vector<double> lamp_positions;
};
// The limit the limiter really works to: current_budget_ma, at most kMaxBudgetMa.
inline double effective_budget_ma(const Settings& s) {
    const double port = s.current_budget_ma < kMaxBudgetMa ? s.current_budget_ma : kMaxBudgetMa;
    // v0.49: minus the board's own current when it shares the port (never below 50 mA for the strip)
    const double esp = s.esp_same_port_ma > 0.0 ? s.esp_same_port_ma : 0.0;
    return port - esp > 50.0 ? port - esp : (port < 50.0 ? port : 50.0);
}
constexpr int kLayoutTv = 0, kLayoutInset = 1, kLayoutCustom = 2;
// True when strip_layout is custom AND lamp_positions fits this lamp count.
bool custom_layout_active(const Settings& s, int lamp_count);
// auto = lamps 1/5 with 5 lamps, else the JointSpace zones touching the bottom (left[0], right[last])
constexpr int kEdgeAuto = 0, kEdgeLamps = 1, kEdgeJointSpace = 2, kEdgeNone = -1;
const char* strip_layout_name(int m);  // "tv" | "inset" | "custom"
int strip_layout_from(const std::string& s, int fallback);
const char* edge_source_name(int m);   // "auto" | "lamps" | "jointspace" | "none" (resolved only)
int edge_source_from(const std::string& s, int fallback);
// v0.30: which lamps are the bottom of the strip (indices into the lamps).
std::vector<int> bottom_lamp_indices(const Settings& s, int lamp_count);
// v0.30: where the edge colour comes from on this frame: kEdgeLamps,
// kEdgeJointSpace or kEdgeNone (inset layout, edge_leds 0, or "lamps" asked
// without 5 lamps).
int edge_source_resolved(const Settings& s, int lamp_count);
// v0.30: does this frame want the JointSpace corner colours (Corners)?
// inset layout: corner_mode side; tv layout: edge colour from JointSpace.
bool wants_side_colours(const Settings& s, int lamp_count);
constexpr int kJsLinear = 0, kJsSrgb = 1;
const char* js_curve_name(int m);  // "linear" | "srgb"
int js_curve_from(const std::string& s, int fallback);
// v0.33: one JointSpace side colour (0..corner_js_full_scale per channel) as LED levels.
RGB8 jointspace_to_led(uint8_t r, uint8_t g, uint8_t b, const Settings& s);
constexpr int kBlendSmooth = 0, kBlendZones = 1, kBlendZonesMid = 2;
constexpr int kCornerLamp = 0, kCornerSide = 1;
constexpr int kDriftSine = 0, kDriftTriangle = 1;
const char* drift_shape_name(int m);  // "sine" | "triangle"
int drift_shape_from(const std::string& s, int fallback);
// v0.24: end_lamps is in effect for this many lamps with these settings.
bool end_lamps_active(const Settings& s, int lamp_count);
const char* blend_mode_name(int m);   // "smooth" | "zones" | "zones_mid"
const char* corner_mode_name(int m);  // "lamp" | "side"
int blend_mode_from(const std::string& s, int fallback);
int corner_mode_from(const std::string& s, int fallback);

// v0.18: the TV's side colours at the two bottom corners, as linear LED
// values (same space as the lamps handed to render_strip). For corner_mode
// kCornerSide, and (v0.30) the tv layout's edge colour from JointSpace.
struct Corners {
    bool has_left = false, has_right = false;
    RGB8 left{}, right{};
    // v0.24 (review item 1): how much of the side colour goes in, 0..1.
    // 1 = the v0.18 half/half mix (byte-identical); the bridge fades this
    // to 0 when JointSpace goes stale instead of switching off at once.
    double weight = 1.0;
};

// What render_strip() did, for the monitor.
struct RenderInfo {
    int used_anchors = 0;
    // "none" (fitted as is), "anchors" (fewer anchors, pe=0), "uniform"
    // (pe=0 anchors were not enough, uniform scale-down), "peaks" (pe>0).
    const char* limit_mode = "none";
    double peak_scale = 1.0;   // smallest per-LED scale at a peak LED (1 = untouched)
    double min_scale = 1.0;    // smallest per-LED scale anywhere
    std::vector<int> peak_leds;  // LED indices of the peaks (pe>0 only)
    // v0.23: estimated current of the full-resolution frame BEFORE any
    // limiting (what the picture wanted), and the drift applied to the
    // peaks on this frame (LEDs, + = towards the end of the strip).
    double wanted_ma = 0.0;
    double peak_offset_leds = 0.0;
    // v0.33: the wear weights took part in this frame's limiting.
    bool wear_applied = false;
};

// v0.33 wear-aware limiter (Settings::wear_strength).
// At wear_strength 1 the most-worn LED's limiter weight is halved.
constexpr double kWearMaxCut = 0.5;
// An LED counts as "fully worn" (wear 1) when its smoothed mAh are this share
// of the strip's mean above the least-used LED. 0.25 is a choice, NOT a
// measurement (the largest difference seen in the logs so far was ~17 %).
constexpr double kWearFullSpread = 0.25;
// Per-LED wear 0..1 from per-LED mAh totals: the first led_count entries
// (missing ones count as the lowest), smoothed over 5 LEDs so single LEDs do
// not stand out, then (mAh - lowest) / (kWearFullSpread * mean), cut to 0..1.
// No data (all zero) -> all 0.
std::vector<double> wear_from_mah(const std::vector<double>& mah, int led_count);

// One lamp's last dispatched colour, for tolerance-based dedup.
struct LampDedupState {
    bool has_value = false;
    double x = 0, y = 0, bri = 0;   // last XY+bri seen (colorspace=1 path)
    uint8_t r = 0, g = 0, b = 0;    // last RGB seen (colorspace=0 path)
};

// Returns true if `next` differs from `prev` by at least `cie_tol` in x/y or
// `bri_tol` in brightness (same shape as ha-hue-entertainment's tolerances,
// CIE_TOLERANCE=0.03 / BRIGHTNESS_TOLERANCE~16-of-255). Updates `prev` when
// it returns true.
bool xy_bri_changed(LampDedupState& prev, double x, double y, double bri_0_254,
                     double cie_tol = 0.03, double bri_tol = 16.0);
bool rgb_changed(LampDedupState& prev, uint8_t r, uint8_t g, uint8_t b, int tol = 16);

// Renders `lamps` (left-to-right anchor colours, as floats 0..255 for smooth
// interpolation) onto `settings.led_count` physical LEDs, shrinking the
// effective anchor resolution (never below `min_anchors`, never above
// lamps.size() upsampled to led_count) until the estimated current fits the
// budget. Returns the chosen anchor count actually used (diagnostic) via
// `used_anchors_out` (may be nullptr).
std::vector<RGB8> render_strip(const std::vector<RGB8>& lamps,
                                const Settings& settings,
                                int* used_anchors_out = nullptr);
// `time_s` (v0.23): a steadily increasing clock in seconds, only used for the
// rolling peaks (peak_drift_leds); 0 = no drift on this frame.
// `wear` (v0.33): per-LED wear 0..1 (wear_from_mah), or nullptr; only used
// when settings.wear_strength > 0, peak_emphasis > 0 and the frame is over
// the budget. Shorter than the strip = ignored.
std::vector<RGB8> render_strip(const std::vector<RGB8>& lamps, const Settings& settings, RenderInfo* info,
                                const Corners* corners = nullptr, double time_s = 0.0,
                                const std::vector<double>* wear = nullptr);

// v0.23: the peak drift (LEDs) render_strip applies at `time_s`.
// v0.24: `lamp_count` is needed for the "auto" amplitude (peak spacing).
double peak_drift_offset(const Settings& settings, double time_s, int lamp_count = 3);
// v0.24: the drift amplitude in LEDs (resolves "auto").
double peak_drift_amplitude(const Settings& settings, int lamp_count);

// v0.24: where the anchor lamps sit (0..1) - lamp_positions(), or the
// end_lamps layout. v0.30 tv layout: one entry per bottom lamp
// (bottom_lamp_indices), evenly from 0 to 1.
std::vector<double> strip_lamp_positions(const Settings& settings, int lamp_count);

// v0.30 tv layout: peak centres in LED units (0..led_count-1 on the strip,
// before rounding), evenly spaced on a ring of led_count LEDs and shifted
// by offset_leds with wrap-around - the gap between neighbouring peaks is
// always led_count / N. Exposed for tests.
std::vector<double> ring_peak_centres(int led_count, int n_peaks, double offset_leds);

// v0.24 (review item 1): after the Hue stream stopped `gap_ms` ago: scale
// the last frame by `factor` (1 = hold, 0 = black); send = false -> stop
// sending (WLED goes back to its own preset after its 2.5 s timeout).
struct LossOutput {
    double factor = 1.0;
    bool send = true;
};
LossOutput stream_loss_output(const Settings& settings, double gap_ms);
// Scales every channel by factor (floor, so the current never rises).
std::vector<RGB8> scale_frame(const std::vector<RGB8>& leds, double factor);
// v0.33: a frame that was rendered EARLIER (the frame held / faded after the
// stream stopped) checked against the settings of NOW: when it estimates above
// the budget (the budget was lowered, or mA per LED raised, while it was held)
// every colour is scaled down by the same factor until it fits; otherwise it
// is returned as it is. Until v0.32 a held frame kept the old budget for up to
// loss_hold_ms + loss_fade_ms.
std::vector<RGB8> fit_budget(const std::vector<RGB8>& leds, const Settings& settings);

// v0.16: positions (0..1 along the strip) of `lamp_count` lamps with the
// given Settings::lamp_inset; one lamp -> 0.5.
std::vector<double> lamp_positions(int lamp_count, double inset);

// Peak weights used by the pe>0 limiter (exposed for tests/monitor):
// w=1 at a peak, 1-0.85*pe halfway between two peaks, raised cosine between.
// v0.23: `offset_leds` shifts every peak by that many LEDs (rolling peaks),
// clamped to the strip; the spacing between peaks stays the same.
std::vector<double> peak_weights(int led_count, int lamp_count, const Settings& settings,
                                  std::vector<int>* peak_leds = nullptr, double offset_leds = 0.0);

// Phase 2(b): rise-rate limit. Returns the next output given the previous
// output and the new target after dt_ms: each channel rises at most
// 255*dt_ms/rise_ms, falls immediately. Size mismatch, rise_ms<=0 or
// dt_ms>=rise_ms*4 (stream gap) -> target unchanged.
std::vector<RGB8> limit_rise(const std::vector<RGB8>& prev, const std::vector<RGB8>& target, double dt_ms,
                             double rise_ms);

// v0.33: what the bridge and the usermod call instead of limit_rise() directly.
// limit_rise() counts in LED values (0 -> 255 in rise_ms), and it lets the
// target through at once when there is no previous frame of the same size or
// after a pause of 4 x rise_ms. Both together meant that the first frame of a
// stream, and the first frame after a pause, went from dark to the full
// budget in ONE step (measured in the sandbox: 0 -> 449.8 mA): a white frame
// that the limiter has already cut to ~40/255 is within one limit_rise step.
// Here, after a start or a pause, the frame moves from what the strip shows
// to the target in proportion, over rise_ms (a "soft start"):
//   - a start = nothing shown yet / another LED count (missing LEDs count as
//     black), a pause = dt_ms > kRiseGapMs or a clock that went backwards;
//     the first step is that of one frame (kRiseGapStepMs);
//   - for the next rise_ms each rising channel may cover at most
//     dt / (time left) of its distance to the target, and never more than
//     limit_rise allows;
//   - after that (RiseState::remaining_ms == 0): exactly limit_rise(), bit
//     for bit - frames that simply flow are not changed.
// Falls are always immediate; rise_ms <= 0 = no limit at all. Never above
// the target, so never above the budget.
constexpr double kRiseGapMs = 100.0;      // 25 fps = 40 ms; more than 2.5 frames late = a pause
constexpr double kRiseGapStepMs = 40.0;   // the step used for the first frame after a start / pause
struct RiseState {
    double remaining_ms = 0.0;  // > 0: soft start in progress
};
// v0.42: start_ms (Settings::fade_in_ms) = the length of the soft start if longer than rise_ms.
std::vector<RGB8> rise_from_shown(RiseState& state, const std::vector<RGB8>& shown, const std::vector<RGB8>& target,
                                  double dt_ms, double rise_ms, double start_ms = 0.0);

// v0.42: a soft fade-out when the picture goes dark. `next` = the frame about to be shown,
// `shown` = the one shown now. When `next` is dark (every channel <= kDarkLevel) and `shown`
// is not, the strip fades from `shown` to `next` over fade_ms ((1-x)^2, like the stream-loss
// fade); any non-dark frame ends the fade at once. Never above `shown`, so never above the
// budget. fade_ms <= 0 = `next` unchanged.
constexpr int kDarkLevel = 3;
struct DarkFadeState {
    bool active = false;
    std::vector<RGB8> from;
    double elapsed_ms = 0.0;
};
bool is_dark(const std::vector<RGB8>& leds);

// v0.44: adaptive smoothing (Settings::smooth_ms). Per channel: tau = smooth_ms * (1 - |change| /
// kSmoothBig), clamped at 0; the value moves 1 - exp(-dt/tau) of the way from `shown` to `next`
// (at least one step). A size mismatch or a pause (dt_ms > kRiseGapMs) = `next` unchanged.
// The result is then fitted to the budget again (channels from two frames together could exceed it).
constexpr double kSmoothBig = 64.0;
std::vector<RGB8> smooth_small_changes(const std::vector<RGB8>& shown, const std::vector<RGB8>& next, double dt_ms,
                                       const Settings& s);
std::vector<RGB8> fade_to_dark(DarkFadeState& state, const std::vector<RGB8>& shown, const std::vector<RGB8>& next,
                               double dt_ms, double fade_ms);

// Estimated total current (mA) for a fully-rendered LED frame, using
// settings.idle_ma_per_led and settings.ma_per_led_full_white. Diagnostic/
// testable in isolation; not a real measurement.
double estimate_current_ma(const std::vector<RGB8>& leds, const Settings& settings);
// v0.33: the one place the current model lives (the limiter, the monitor and
// strip_stats all use it). led_colour_ma = the colour part of one LED,
// led_current_ma = that plus its idle draw.
double led_colour_ma(const RGB8& c, const Settings& settings);
inline double led_current_ma(const RGB8& c, const Settings& settings) {
    return settings.idle_ma_per_led + led_colour_ma(c, settings);
}

}  // namespace color_pipeline
