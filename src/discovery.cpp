#include "discovery.hpp"

#include <cjson/cJSON.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace discovery {

static std::string lower_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    std::string t = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
    return t;
}

std::vector<int> answer_variants(const std::string& st, const std::string& udn, bool hue_only) {
    if (!hue_only) return {0, 1, 2};
    std::string s = lower_trim(st);
    if (s == "ssdp:all") return {0, 1, 2};
    if (s == "upnp:rootdevice") return {0};
    if (s == lower_trim("uuid:" + udn)) return {1};
    if (s == "urn:schemas-upnp-org:device:basic:1") return {2};
    return {};
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Registry::Registry(std::string state_dir) : state_dir_(std::move(state_dir)) {
    if (state_dir_.empty()) return;
    std::ifstream f(state_dir_ + "/discovery.json");
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    cJSON* root = text.size() < 4096 ? cJSON_ParseWithLength(text.c_str(), text.size()) : nullptr;
    if (!root) return;
    cJSON* h = cJSON_GetObjectItemCaseSensitive(root, "ssdp_hue_only");
    hue_only_ = cJSON_IsTrue(h);
    cJSON_Delete(root);
}

bool Registry::hue_only() const {
    std::lock_guard<std::mutex> lk(mu_);
    return hue_only_;
}

bool Registry::set_hue_only(bool on, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!state_dir_.empty()) {
        std::string path = state_dir_ + "/discovery.json";
        std::string tmp = path + ".tmp";
        std::string text = std::string("{\n\t\"ssdp_hue_only\":\t") + (on ? "true" : "false") + "\n}";
        bool ok = false;
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
            ok = (std::fclose(f) == 0) && ok;
        }
        if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
            if (err) *err = "save_failed";
            return false;
        }
    }
    hue_only_ = on;
    return true;
}

void Registry::note(const std::string& ip, const std::string& kind, const std::string& what,
                    const std::string& ua, bool answered, int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& a : askers_) {
        if (a.ip == ip && a.kind == kind && a.what == what && a.ua == ua) {
            ++a.count;
            a.last_ms = now;
            a.answered = answered;
            return;
        }
    }
    if (askers_.size() >= kMaxEntries) {
        auto oldest = std::min_element(askers_.begin(), askers_.end(),
                                       [](const Asker& x, const Asker& y) { return x.last_ms < y.last_ms; });
        askers_.erase(oldest);
    }
    askers_.push_back({ip, kind, what, ua, 1, now, now, answered});
}

std::vector<Asker> Registry::snapshot() const {
    std::vector<Asker> v;
    {
        std::lock_guard<std::mutex> lk(mu_);
        v = askers_;
    }
    std::stable_sort(v.begin(), v.end(), [](const Asker& x, const Asker& y) { return x.last_ms > y.last_ms; });
    return v;
}

}  // namespace discovery
