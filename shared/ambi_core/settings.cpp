#include "settings.hpp"
#if __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#include <cJSON.h>  // ESP-IDF "json" component
#endif
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace settings {

using color_pipeline::Settings;

static double get_num(cJSON* obj, const char* key, double fallback) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    return fallback;
}

static bool get_bool(cJSON* obj, const char* key, bool fallback) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return fallback;
}

static double finite_or(double v, double fallback) { return std::isfinite(v) ? v : fallback; }

void sanitize(Settings& s) {
    const Settings d;
    auto clampd = [](double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); };
    s.led_count = std::max(1, std::min(s.led_count, 1000));
    s.current_budget_ma = clampd(finite_or(s.current_budget_ma, d.current_budget_ma), 0.0, color_pipeline::kMaxBudgetMa);  // v0.33: build-time ceiling
    s.idle_ma_per_led = clampd(finite_or(s.idle_ma_per_led, d.idle_ma_per_led), 0.0, 20.0);
    s.ma_per_led_full_white = clampd(finite_or(s.ma_per_led_full_white, d.ma_per_led_full_white), 1.0, 200.0);
    // v0.33: per-channel share of the current (1 = as v0.32). Not measured values.
    s.ma_scale_r = clampd(finite_or(s.ma_scale_r, d.ma_scale_r), 0.1, 3.0);
    s.ma_scale_g = clampd(finite_or(s.ma_scale_g, d.ma_scale_g), 0.1, 3.0);
    s.ma_scale_b = clampd(finite_or(s.ma_scale_b, d.ma_scale_b), 0.1, 3.0);
    s.min_anchors = std::max(1, std::min(s.min_anchors, 1000));
    s.peak_emphasis = clampd(finite_or(s.peak_emphasis, d.peak_emphasis), 0.0, 1.0);
    s.peak_points = std::max(0, std::min(1000, s.peak_points));
    s.rise_ms = clampd(finite_or(s.rise_ms, d.rise_ms), 0.0, 10000.0);
    s.fade_in_ms = clampd(finite_or(s.fade_in_ms, d.fade_in_ms), 0.0, 10000.0);    // v0.42
    s.fade_out_ms = clampd(finite_or(s.fade_out_ms, d.fade_out_ms), 0.0, 10000.0);
    s.smooth_ms = clampd(finite_or(s.smooth_ms, d.smooth_ms), 0.0, 2000.0);  // v0.44
    s.lamp_inset = clampd(finite_or(s.lamp_inset, d.lamp_inset), 0.0, 0.45);
    if (s.blend_mode < color_pipeline::kBlendSmooth || s.blend_mode > color_pipeline::kBlendZonesMid) s.blend_mode = d.blend_mode;
    if (s.corner_mode != color_pipeline::kCornerLamp && s.corner_mode != color_pipeline::kCornerSide) s.corner_mode = d.corner_mode;
    s.blend_softness = clampd(finite_or(s.blend_softness, d.blend_softness), 0.0, 1.0);
    // v0.24: any negative amplitude = "auto" (stored as -1)
    s.peak_drift_leds = finite_or(s.peak_drift_leds, d.peak_drift_leds);
    s.peak_drift_leds = s.peak_drift_leds < 0.0 ? -1.0 : std::min(s.peak_drift_leds, 20.0);
    s.peak_drift_period_s = clampd(finite_or(s.peak_drift_period_s, d.peak_drift_period_s), 5.0, 3600.0);
    if (s.peak_drift_shape != color_pipeline::kDriftSine && s.peak_drift_shape != color_pipeline::kDriftTriangle)
        s.peak_drift_shape = d.peak_drift_shape;
    s.loss_hold_ms = clampd(finite_or(s.loss_hold_ms, d.loss_hold_ms), 0.0, 10000.0);
    s.loss_fade_ms = clampd(finite_or(s.loss_fade_ms, d.loss_fade_ms), 0.0, 10000.0);
    s.loss_black_s = clampd(finite_or(s.loss_black_s, d.loss_black_s), 0.0, 3600.0);
    // v0.30
    if (s.strip_layout != color_pipeline::kLayoutTv && s.strip_layout != color_pipeline::kLayoutInset &&
        s.strip_layout != color_pipeline::kLayoutCustom)
        s.strip_layout = d.strip_layout;
    // v0.33
    s.wear_strength = clampd(finite_or(s.wear_strength, d.wear_strength), 0.0, 1.0);
    if (s.corner_js_curve != color_pipeline::kJsLinear && s.corner_js_curve != color_pipeline::kJsSrgb)
        s.corner_js_curve = d.corner_js_curve;
    s.corner_js_full_scale = clampd(finite_or(s.corner_js_full_scale, d.corner_js_full_scale), 1.0, 255.0);
    // v0.32
    s.brightness = clampd(finite_or(s.brightness, d.brightness), 0.0, 1.0);
    s.wb_r = clampd(finite_or(s.wb_r, d.wb_r), 0.2, 1.0);  // v0.48: white balance
    s.wb_g = clampd(finite_or(s.wb_g, d.wb_g), 0.2, 1.0);
    s.wb_b = clampd(finite_or(s.wb_b, d.wb_b), 0.2, 1.0);
    if (s.lamp_positions.size() > 10) s.lamp_positions.resize(10);  // the TV streams at most 10 lamps
    for (double& p : s.lamp_positions) p = !std::isfinite(p) || p < 0.0 ? -1.0 : std::min(1.0, p);
    s.edge_leds = std::max(0, std::min(10, s.edge_leds));
    s.edge_strength = clampd(finite_or(s.edge_strength, d.edge_strength), 0.0, 1.0);
    if (s.edge_source != color_pipeline::kEdgeAuto && s.edge_source != color_pipeline::kEdgeLamps &&
        s.edge_source != color_pipeline::kEdgeJointSpace)
        s.edge_source = d.edge_source;
}

cJSON* to_json(const Settings& s) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "led_count", s.led_count);
    cJSON_AddBoolToObject(root, "diffuser", s.diffuser);
    cJSON_AddNumberToObject(root, "current_budget_ma", s.current_budget_ma);
    cJSON_AddNumberToObject(root, "idle_ma_per_led", s.idle_ma_per_led);
    cJSON_AddNumberToObject(root, "ma_per_led_full_white", s.ma_per_led_full_white);
    cJSON_AddNumberToObject(root, "ma_scale_r", s.ma_scale_r);
    cJSON_AddNumberToObject(root, "ma_scale_g", s.ma_scale_g);
    cJSON_AddNumberToObject(root, "ma_scale_b", s.ma_scale_b);
    cJSON_AddNumberToObject(root, "min_anchors", s.min_anchors);
    cJSON_AddNumberToObject(root, "peak_emphasis", s.peak_emphasis);
    cJSON_AddNumberToObject(root, "peak_points", s.peak_points);
    cJSON_AddNumberToObject(root, "rise_ms", s.rise_ms);
    cJSON_AddNumberToObject(root, "fade_in_ms", s.fade_in_ms);
    cJSON_AddNumberToObject(root, "fade_out_ms", s.fade_out_ms);
    cJSON_AddNumberToObject(root, "smooth_ms", s.smooth_ms);
    cJSON_AddNumberToObject(root, "lamp_inset", s.lamp_inset);
    cJSON_AddStringToObject(root, "blend_mode", color_pipeline::blend_mode_name(s.blend_mode));
    cJSON_AddStringToObject(root, "corner_mode", color_pipeline::corner_mode_name(s.corner_mode));
    cJSON_AddNumberToObject(root, "blend_softness", s.blend_softness);
    if (s.peak_drift_leds < 0.0) cJSON_AddStringToObject(root, "peak_drift_leds", "auto");
    else cJSON_AddNumberToObject(root, "peak_drift_leds", s.peak_drift_leds);
    cJSON_AddNumberToObject(root, "peak_drift_period_s", s.peak_drift_period_s);
    cJSON_AddStringToObject(root, "peak_drift_shape", color_pipeline::drift_shape_name(s.peak_drift_shape));
    cJSON_AddBoolToObject(root, "end_lamps", s.end_lamps);
    cJSON_AddBoolToObject(root, "wb_keep_pure", s.wb_keep_pure);  // v0.49
    cJSON_AddNumberToObject(root, "loss_hold_ms", s.loss_hold_ms);
    cJSON_AddNumberToObject(root, "loss_fade_ms", s.loss_fade_ms);
    cJSON_AddNumberToObject(root, "loss_black_s", s.loss_black_s);
    cJSON_AddStringToObject(root, "strip_layout", color_pipeline::strip_layout_name(s.strip_layout));
    cJSON_AddNumberToObject(root, "edge_leds", s.edge_leds);
    cJSON_AddNumberToObject(root, "edge_strength", s.edge_strength);
    cJSON_AddStringToObject(root, "edge_source", color_pipeline::edge_source_name(s.edge_source));
    cJSON_AddNumberToObject(root, "brightness", s.brightness);
    cJSON_AddNumberToObject(root, "wb_r", s.wb_r);
    cJSON_AddNumberToObject(root, "wb_g", s.wb_g);
    cJSON_AddNumberToObject(root, "wb_b", s.wb_b);
    cJSON_AddNumberToObject(root, "wear_strength", s.wear_strength);
    cJSON_AddStringToObject(root, "corner_js_curve", color_pipeline::js_curve_name(s.corner_js_curve));
    cJSON_AddNumberToObject(root, "corner_js_full_scale", s.corner_js_full_scale);
    {
        cJSON* lp = cJSON_CreateArray();
        for (double p : s.lamp_positions) cJSON_AddItemToArray(lp, cJSON_CreateNumber(p));
        cJSON_AddItemToObject(root, "lamp_positions", lp);
    }
    return root;
}

void merge_json(Settings& s, const cJSON* obj) {
    cJSON* root = const_cast<cJSON*>(obj);
    if (cJSON_IsObject(root)) {
        // Integers are read through valuedouble and clamped before the cast,
        // so a huge number can't overflow an int.
        auto get_int = [&](const char* key, int fallback) {
            double v = get_num(root, key, fallback);
            if (!std::isfinite(v)) return fallback;
            return static_cast<int>(std::max(-1e6, std::min(1e6, v)));
        };
        s.led_count = get_int("led_count", s.led_count);
        s.diffuser = get_bool(root, "diffuser", s.diffuser);
        s.current_budget_ma = get_num(root, "current_budget_ma", s.current_budget_ma);
        s.idle_ma_per_led = get_num(root, "idle_ma_per_led", s.idle_ma_per_led);
        s.ma_per_led_full_white = get_num(root, "ma_per_led_full_white", s.ma_per_led_full_white);
        s.ma_scale_r = get_num(root, "ma_scale_r", s.ma_scale_r);
        s.ma_scale_g = get_num(root, "ma_scale_g", s.ma_scale_g);
        s.ma_scale_b = get_num(root, "ma_scale_b", s.ma_scale_b);
        s.min_anchors = get_int("min_anchors", s.min_anchors);
        s.peak_emphasis = get_num(root, "peak_emphasis", s.peak_emphasis);
        s.peak_points = get_int("peak_points", s.peak_points);
        s.rise_ms = get_num(root, "rise_ms", s.rise_ms);
        s.fade_in_ms = get_num(root, "fade_in_ms", s.fade_in_ms);
        s.fade_out_ms = get_num(root, "fade_out_ms", s.fade_out_ms);
        s.smooth_ms = get_num(root, "smooth_ms", s.smooth_ms);
        s.lamp_inset = get_num(root, "lamp_inset", s.lamp_inset);
        s.blend_softness = get_num(root, "blend_softness", s.blend_softness);
        s.peak_drift_leds = get_num(root, "peak_drift_leds", s.peak_drift_leds);
        cJSON* dl = cJSON_GetObjectItemCaseSensitive(root, "peak_drift_leds");
        if (cJSON_IsString(dl) && dl->valuestring && std::string(dl->valuestring) == "auto") s.peak_drift_leds = -1.0;
        s.peak_drift_period_s = get_num(root, "peak_drift_period_s", s.peak_drift_period_s);
        cJSON* ds = cJSON_GetObjectItemCaseSensitive(root, "peak_drift_shape");
        if (cJSON_IsString(ds) && ds->valuestring)
            s.peak_drift_shape = color_pipeline::drift_shape_from(ds->valuestring, s.peak_drift_shape);
        s.end_lamps = get_bool(root, "end_lamps", s.end_lamps);
        s.wb_keep_pure = get_bool(root, "wb_keep_pure", s.wb_keep_pure);
        s.loss_hold_ms = get_num(root, "loss_hold_ms", s.loss_hold_ms);
        s.loss_fade_ms = get_num(root, "loss_fade_ms", s.loss_fade_ms);
        s.loss_black_s = get_num(root, "loss_black_s", s.loss_black_s);
        cJSON* bm = cJSON_GetObjectItemCaseSensitive(root, "blend_mode");
        if (cJSON_IsString(bm) && bm->valuestring) s.blend_mode = color_pipeline::blend_mode_from(bm->valuestring, s.blend_mode);
        cJSON* sl = cJSON_GetObjectItemCaseSensitive(root, "strip_layout");
        if (cJSON_IsString(sl) && sl->valuestring)
            s.strip_layout = color_pipeline::strip_layout_from(sl->valuestring, s.strip_layout);
        s.edge_leds = get_int("edge_leds", s.edge_leds);
        s.edge_strength = get_num(root, "edge_strength", s.edge_strength);
        cJSON* es = cJSON_GetObjectItemCaseSensitive(root, "edge_source");
        if (cJSON_IsString(es) && es->valuestring)
            s.edge_source = color_pipeline::edge_source_from(es->valuestring, s.edge_source);
        s.brightness = get_num(root, "brightness", s.brightness);
        s.wb_r = get_num(root, "wb_r", s.wb_r);
        s.wb_g = get_num(root, "wb_g", s.wb_g);
        s.wb_b = get_num(root, "wb_b", s.wb_b);
        s.wear_strength = get_num(root, "wear_strength", s.wear_strength);
        cJSON* jc = cJSON_GetObjectItemCaseSensitive(root, "corner_js_curve");
        if (cJSON_IsString(jc) && jc->valuestring)
            s.corner_js_curve = color_pipeline::js_curve_from(jc->valuestring, s.corner_js_curve);
        s.corner_js_full_scale = get_num(root, "corner_js_full_scale", s.corner_js_full_scale);
        cJSON* lp = cJSON_GetObjectItemCaseSensitive(root, "lamp_positions");
        if (cJSON_IsArray(lp)) {
            // all numbers, or the list is not taken at all (no half-applied map)
            std::vector<double> v;
            bool ok = true;
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, lp) {
                if (cJSON_IsNumber(it)) v.push_back(it->valuedouble);
                else ok = false;
            }
            if (ok && v.size() <= 10) s.lamp_positions = v;
        }
        cJSON* cm = cJSON_GetObjectItemCaseSensitive(root, "corner_mode");
        if (cJSON_IsString(cm) && cm->valuestring) s.corner_mode = color_pipeline::corner_mode_from(cm->valuestring, s.corner_mode);
    }
    sanitize(s);
}

std::vector<ValidationError> validate_json(const cJSON* obj) {
    std::vector<ValidationError> errs;
    if (!cJSON_IsObject(obj)) {
        errs.push_back({"", "a JSON object is needed"});
        return errs;
    }
    struct Num {
        const char* name;
        double lo, hi;
        bool whole;
    };
    // The same limits sanitize() clamps to.
    static const Num nums[] = {
        {"led_count", 1, 1000, true},
        {"current_budget_ma", 0, color_pipeline::kMaxBudgetMa, false},  // v0.33
        {"idle_ma_per_led", 0, 20, false},
        {"ma_per_led_full_white", 1, 200, false},
        {"ma_scale_r", 0.1, 3, false},
        {"ma_scale_g", 0.1, 3, false},
        {"ma_scale_b", 0.1, 3, false},
        {"min_anchors", 1, 1000, true},
        {"peak_emphasis", 0, 1, false},
        {"peak_points", 0, 1000, true},
        {"rise_ms", 0, 10000, false},
        {"fade_in_ms", 0, 10000, false},   // v0.42
        {"fade_out_ms", 0, 10000, false},
        {"smooth_ms", 0, 2000, false},     // v0.44
        {"lamp_inset", 0, 0.45, false},
        {"blend_softness", 0, 1, false},
        {"peak_drift_leds", -1, 20, false},
        {"peak_drift_period_s", 5, 3600, false},
        {"loss_hold_ms", 0, 10000, false},
        {"loss_fade_ms", 0, 10000, false},
        {"loss_black_s", 0, 3600, false},
        {"edge_leds", 0, 10, true},
        {"edge_strength", 0, 1, false},
        {"brightness", 0, 1, false},
        {"wb_r", 0.2, 1, false},  // v0.48
        {"wb_g", 0.2, 1, false},
        {"wb_b", 0.2, 1, false},
        {"corner_js_full_scale", 1, 255, false},
        {"wear_strength", 0, 1, false},
    };
    struct Choice {
        const char* name;
        std::vector<std::string> values;
    };
    static const Choice choices[] = {
        {"strip_layout", {"tv", "inset", "custom"}},
        {"blend_mode", {"smooth", "zones", "zones_mid"}},
        {"corner_mode", {"lamp", "side"}},
        {"edge_source", {"auto", "lamps", "jointspace"}},
        {"peak_drift_shape", {"sine", "triangle"}},
        {"corner_js_curve", {"linear", "srgb"}},
    };
    auto fmt = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        return std::string(buf);
    };
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, obj) {
        const std::string key = it->string ? it->string : "";
        bool known = false;
        for (const Num& n : nums) {
            if (key != n.name) continue;
            known = true;
            // peak_drift_leds is also written, and accepted, as "auto"
            if (key == "peak_drift_leds" && cJSON_IsString(it) && it->valuestring && std::string(it->valuestring) == "auto") continue;
            if (!cJSON_IsNumber(it) || !std::isfinite(it->valuedouble)) {
                errs.push_back({key, "a number is needed"});
            } else if (it->valuedouble < n.lo || it->valuedouble > n.hi) {
                errs.push_back({key, "must be between " + fmt(n.lo) + " and " + fmt(n.hi)});
            } else if (n.whole && it->valuedouble != std::floor(it->valuedouble)) {
                errs.push_back({key, "must be a whole number"});
            }
        }
        for (const Choice& c : choices) {
            if (key != c.name) continue;
            known = true;
            std::string list;
            for (const auto& v : c.values) list += (list.empty() ? "" : ", ") + v;
            if (!cJSON_IsString(it) || !it->valuestring ||
                std::find(c.values.begin(), c.values.end(), it->valuestring) == c.values.end())
                errs.push_back({key, "must be one of: " + list});
        }
        if (key == "diffuser" || key == "end_lamps" || key == "wb_keep_pure") {
            known = true;
            if (!cJSON_IsBool(it)) errs.push_back({key, "true or false is needed"});
        }
        if (key == "lamp_positions") {
            known = true;
            if (!cJSON_IsArray(it)) {
                errs.push_back({key, "a list of numbers is needed"});
            } else if (cJSON_GetArraySize(it) > 10) {
                errs.push_back({key, "at most 10 lamps"});
            } else {
                int index = 0;
                const cJSON* p = nullptr;
                cJSON_ArrayForEach(p, it) {
                    ++index;
                    if (!cJSON_IsNumber(p) || !std::isfinite(p->valuedouble)) {
                        errs.push_back({key, "lamp " + std::to_string(index) + ": a number is needed"});
                    } else if (p->valuedouble > 1.0) {
                        errs.push_back({key, "lamp " + std::to_string(index) + ": must be 0..1, or negative for a lamp that is not shown"});
                    }
                }
            }
        }
        if (!known) errs.push_back({key, "unknown setting"});
    }
    return errs;
}

Settings load(const std::string& path) {
    Settings s;  // defaults
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return s;

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) {  // sane upper bound; never trust a huge file
        std::fclose(f);
        return s;
    }
    std::string buf(static_cast<size_t>(size), '\0');
    size_t read = std::fread(&buf[0], 1, static_cast<size_t>(size), f);
    std::fclose(f);
    if (read != static_cast<size_t>(size)) return s;

    cJSON* root = cJSON_ParseWithLength(buf.c_str(), buf.size());
    if (!root) return s;  // malformed JSON -> defaults, never crash
    merge_json(s, root);
    cJSON_Delete(root);
    return s;
}

bool save(const Settings& s, const std::string& path) {
    cJSON* root = to_json(s);
    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;

    // Write to a temp file then rename, so a crash mid-write never corrupts
    // the settings file the bridge will try to load on next boot.
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        cJSON_free(text);
        return false;
    }
    size_t len = std::char_traits<char>::length(text);
    bool ok = std::fwrite(text, 1, len, f) == len;
    std::fclose(f);
    cJSON_free(text);
    if (!ok) return false;
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace settings
