// v0.32: the strict settings check behind the monitor's /ambi/settings page,
// and that the new settings (brightness, lamp_positions, layout "custom")
// survive settings.json.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <cjson/cJSON.h>

#include "settings.hpp"

namespace {
[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}
std::vector<settings::ValidationError> check(const char* json) {
    cJSON* j = cJSON_Parse(json);
    auto e = settings::validate_json(j);
    if (j) cJSON_Delete(j);
    return e;
}
bool has(const std::vector<settings::ValidationError>& e, const std::string& field) {
    for (const auto& x : e)
        if (x.field == field) return true;
    return false;
}
}  // namespace

int main() {
    // ---- defaults the page shows --------------------------------------------
    const color_pipeline::Settings d;
    if (d.current_budget_ma != 450.0) fail("default current limit must be 450 mA");
    if (d.ma_per_led_full_white != 45.0) fail("default mA per LED must be 45 (v0.34, measured)");
    if (d.brightness != 1.0 || !d.lamp_positions.empty()) fail("default brightness 1, no custom positions");

    // ---- good values pass, one by one and together ------------------------------
    if (!check(R"({"led_count":43,"ma_per_led_full_white":60,"current_budget_ma":450,"brightness":0.8,"rise_ms":120,
                   "peak_emphasis":0.5,"strip_layout":"custom","lamp_positions":[0,0.5,1]})").empty())
        fail("a correct form was refused");
    if (!check(R"({})").empty()) fail("an empty change is fine");
    if (!check(R"({"lamp_positions":[]})").empty()) fail("an empty position list is fine");
    if (!check(R"({"lamp_positions":[0.1,-1,0.9]})").empty()) fail("-1 = lamp not shown is fine");
    if (!check(R"({"current_budget_ma":0})").empty() || !check(R"({"current_budget_ma":900})").empty()) fail("limits themselves are allowed");
    {
        // everything to_json() writes must pass the check (the old panel posts all of it)
        cJSON* all = settings::to_json(d);
        if (!settings::validate_json(all).empty()) fail("the defaults as written by to_json() do not pass validate_json(): " + settings::validate_json(all)[0].field);
        cJSON_Delete(all);
    }
    std::fprintf(stderr, "[ok] defaults 450 mA / 45 mA per LED; correct values and to_json() output pass\n");

    // ---- wrong values: each is reported under its own field -----------------------
    struct Bad {
        const char* json;
        const char* field;
    };
    const Bad bad[] = {
        {R"({"led_count":0})", "led_count"},
        {R"({"led_count":1001})", "led_count"},
        {R"({"led_count":43.5})", "led_count"},
        {R"({"led_count":"43"})", "led_count"},
        {R"({"led_count":null})", "led_count"},
        {R"({"ma_per_led_full_white":0})", "ma_per_led_full_white"},
        {R"({"ma_per_led_full_white":250})", "ma_per_led_full_white"},
        {R"({"current_budget_ma":-1})", "current_budget_ma"},
        {R"({"current_budget_ma":901})", "current_budget_ma"},
        {R"({"brightness":1.5})", "brightness"},
        {R"({"brightness":-0.1})", "brightness"},
        {R"({"rise_ms":-5})", "rise_ms"},
        {R"({"rise_ms":10001})", "rise_ms"},
        {R"({"peak_emphasis":2})", "peak_emphasis"},
        {R"({"strip_layout":"sideways"})", "strip_layout"},
        {R"({"strip_layout":2})", "strip_layout"},
        {R"({"lamp_positions":"0,1"})", "lamp_positions"},
        {R"({"lamp_positions":[0,"x",1]})", "lamp_positions"},
        {R"({"lamp_positions":[0,1.2]})", "lamp_positions"},
        {R"({"lamp_positions":[0,0,0,0,0,0,0,0,0,0,0]})", "lamp_positions"},
        {R"({"curent_budget_ma":450})", "curent_budget_ma"},  // a typo is not ignored
        {R"({"diffuser":"yes"})", "diffuser"},
    };
    for (const Bad& b : bad) {
        const auto e = check(b.json);
        if (!has(e, b.field)) fail(std::string("not refused: ") + b.json);
        if (e[0].message.empty()) fail("an error needs a message");
    }
    if (check(R"({"led_count":0,"brightness":9,"rise_ms":-1})").size() != 3) fail("every wrong field must be listed, not only the first");
    if (check("[1,2]").empty() || check("nonsense").empty()) fail("not an object must be refused");
    std::fprintf(stderr, "[ok] 22 wrong values each refused under their own field; all errors listed\n");

    // ---- merge + sanitize + settings.json round trip --------------------------------
    const std::string path = "/tmp/ambi_test_settings_validation.json";
    std::remove(path.c_str());
    color_pipeline::Settings s;
    cJSON* j = cJSON_Parse(R"({"brightness":0.4,"strip_layout":"custom","lamp_positions":[1,-3,0.25],"current_budget_ma":300})");
    settings::merge_json(s, j);
    cJSON_Delete(j);
    if (s.brightness != 0.4 || s.strip_layout != color_pipeline::kLayoutCustom || s.current_budget_ma != 300.0) fail("merge_json did not take the new settings");
    if (s.lamp_positions.size() != 3 || s.lamp_positions[1] != -1.0 || s.lamp_positions[2] != 0.25) fail("lamp_positions: negative must be stored as -1");
    if (!settings::save(s, path)) fail("save failed");
    const color_pipeline::Settings r = settings::load(path);
    if (r.brightness != 0.4 || r.strip_layout != color_pipeline::kLayoutCustom || r.lamp_positions != s.lamp_positions || r.current_budget_ma != 300.0)
        fail("settings.json round trip lost a v0.32 setting");
    // a mixed list is not half-applied
    j = cJSON_Parse(R"({"lamp_positions":[0,"x",1]})");
    settings::merge_json(s, j);
    cJSON_Delete(j);
    if (s.lamp_positions.size() != 3 || s.lamp_positions[0] != 1.0) fail("a broken lamp_positions list must leave the old one in place");
    // a settings.json from before v0.32 (no brightness / lamp_positions) loads with the defaults for them
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        std::fputs(R"({"led_count":39,"current_budget_ma":800,"strip_layout":"tv"})", f);
        std::fclose(f);
        const color_pipeline::Settings old = settings::load(path);
        if (old.led_count != 39 || old.current_budget_ma != 800.0) fail("an older settings.json must keep its own values (800 mA stays 800)");
        if (old.brightness != 1.0 || !old.lamp_positions.empty()) fail("missing v0.32 settings must get their defaults");
    }
    std::fprintf(stderr, "[ok] merge + settings.json round trip; a broken list changes nothing; an older file keeps its 800 mA\n");
    std::printf("ALL SETTINGS VALIDATION TESTS PASSED\n");
    return 0;
}
