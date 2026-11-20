#include "identity_store.hpp"

#include <cjson/cJSON.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "bridge_identity.hpp"

namespace identity_store {

const Entry* Store::find(const std::string& mac) const {
    for (const auto& e : entries)
        if (e.mac == mac) return &e;
    return nullptr;
}

bool valid_mac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (size_t i = 0; i < mac.size(); ++i) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(mac[i]))) {
            return false;
        }
    }
    return true;
}

std::string normalize_mac(const std::string& mac) {
    std::string m = mac;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::tolower(c); });
    return m;
}

bool valid_name(const std::string& name, std::string* err) {
    auto fail = [err](const char* why) {
        if (err) *err = why;
        return false;
    };
    if (name.empty()) return fail("name_empty");
    if (name.size() > 32) return fail("name_too_long");
    if (name.front() == ' ' || name.back() == ' ') return fail("name_spaces_at_ends");
    for (unsigned char c : name) {
        if (c < 0x20 || c > 0x7e) return fail("name_not_ascii");
        if (c == '<' || c == '>' || c == '&' || c == '"' || c == '\'') return fail("name_bad_character");
    }
    return true;
}

std::string default_name_for(const std::string& mac) {
    std::string hex = bridge_identity::mac_to_hex(mac);
    std::string tail = hex.size() >= 6 ? hex.substr(hex.size() - 6) : hex;
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return std::toupper(c); });
    return "FakeBridge-" + tail;
}

std::string random_mac() {
    unsigned char b[3] = {0, 0, 0};
    std::ifstream ur("/dev/urandom", std::ios::binary);
    ur.read(reinterpret_cast<char*>(b), sizeof(b));
    char buf[18];
    std::snprintf(buf, sizeof(buf), "00:17:88:%02x:%02x:%02x", b[0], b[1], b[2]);
    return buf;
}

static Store default_store() {
    Store s;
    s.entries.push_back({bridge_identity::kDefaultMac, bridge_identity::kDefaultName, 3});
    s.active = bridge_identity::kDefaultMac;
    return s;
}

Store load(const std::string& state_dir) {
    Store s;
    std::ifstream f(state_dir + "/identity.json");
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        cJSON* root = text.size() < 65536 ? cJSON_ParseWithLength(text.c_str(), text.size()) : nullptr;
        if (root) {
            cJSON* list = cJSON_GetObjectItemCaseSensitive(root, "identities");
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, list) {
                cJSON* m = cJSON_GetObjectItemCaseSensitive(it, "mac");
                cJSON* n = cJSON_GetObjectItemCaseSensitive(it, "name");
                cJSON* l = cJSON_GetObjectItemCaseSensitive(it, "lamps");
                if (!cJSON_IsString(m) || !valid_mac(m->valuestring)) continue;
                Entry e;
                e.mac = normalize_mac(m->valuestring);
                if (s.find(e.mac)) continue;  // duplicate
                e.name = cJSON_IsString(n) && valid_name(n->valuestring, nullptr) ? n->valuestring
                                                                                   : default_name_for(e.mac);
                e.lamps = cJSON_IsNumber(l) && l->valueint == 5 ? 5 : 3;
                s.entries.push_back(e);
            }
            cJSON* a = cJSON_GetObjectItemCaseSensitive(root, "active");
            if (cJSON_IsString(a) && valid_mac(a->valuestring)) s.active = normalize_mac(a->valuestring);
            cJSON_Delete(root);
        }
    }
    // The default identity is always there (it owns the existing pairing).
    if (!s.find(bridge_identity::kDefaultMac)) {
        s.entries.insert(s.entries.begin(), default_store().entries.front());
    }
    if (!s.find(s.active)) s.active = bridge_identity::kDefaultMac;
    return s;
}

bool save(const Store& s, const std::string& state_dir) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active", s.active.c_str());
    cJSON* list = cJSON_AddArrayToObject(root, "identities");
    for (const auto& e : s.entries) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "mac", e.mac.c_str());
        cJSON_AddStringToObject(o, "name", e.name.c_str());
        cJSON_AddNumberToObject(o, "lamps", e.lamps);
        cJSON_AddItemToArray(list, o);
    }
    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;
    std::string path = state_dir + "/identity.json";
    std::string tmp = path + ".tmp";
    bool ok = false;
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        size_t len = std::char_traits<char>::length(text);
        ok = std::fwrite(text, 1, len, f) == len;
        ok = (std::fclose(f) == 0) && ok;
    }
    cJSON_free(text);
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string dir_for(const std::string& state_dir, const std::string& mac) {
    if (normalize_mac(mac) == bridge_identity::kDefaultMac) return state_dir;
    return state_dir + "/identities/" + bridge_identity::mac_to_hex(mac);
}

Shared::Shared(std::string state_dir, Entry running)
    : state_dir_(std::move(state_dir)), running_(std::move(running)) {
    store_ = load(state_dir_);
}

Store Shared::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return store_;
}

bool Shared::select(const std::string& mac, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string m = normalize_mac(mac);
    if (!valid_mac(m) || !store_.find(m)) {
        if (err) *err = "unknown_identity";
        return false;
    }
    Store next = store_;
    next.active = m;
    if (!save(next, state_dir_)) {
        if (err) *err = "save_failed";
        return false;
    }
    store_ = next;
    return true;
}

bool Shared::rename(const std::string& mac, const std::string& name, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string m = normalize_mac(mac);
    if (!valid_name(name, err)) return false;
    Store next = store_;
    auto it = std::find_if(next.entries.begin(), next.entries.end(), [&](const Entry& e) { return e.mac == m; });
    if (it == next.entries.end()) {
        if (err) *err = "unknown_identity";
        return false;
    }
    it->name = name;
    if (!save(next, state_dir_)) {
        if (err) *err = "save_failed";
        return false;
    }
    store_ = next;
    return true;
}

bool Shared::set_lamps(const std::string& mac, int lamps, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (lamps != 3 && lamps != 5) {
        if (err) *err = "lamps_must_be_3_or_5";
        return false;
    }
    std::string m = normalize_mac(mac);
    Store next = store_;
    auto it = std::find_if(next.entries.begin(), next.entries.end(), [&](const Entry& e) { return e.mac == m; });
    if (it == next.entries.end()) {
        if (err) *err = "unknown_identity";
        return false;
    }
    it->lamps = lamps;
    if (!save(next, state_dir_)) {
        if (err) *err = "save_failed";
        return false;
    }
    store_ = next;
    return true;
}

bool Shared::create(const std::string& name, std::string* new_mac, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (store_.entries.size() >= 20) {
        if (err) *err = "too_many_identities";
        return false;
    }
    std::string mac;
    for (int tries = 0; tries < 20; ++tries) {
        mac = random_mac();
        if (!store_.find(mac) && mac != bridge_identity::kDefaultMac) break;
        mac.clear();
    }
    if (mac.empty()) {
        if (err) *err = "no_free_mac";
        return false;
    }
    Entry e{mac, name.empty() ? default_name_for(mac) : name, 3};
    if (!valid_name(e.name, err)) return false;
    Store next = store_;
    next.entries.push_back(e);
    next.active = mac;
    if (!save(next, state_dir_)) {
        if (err) *err = "save_failed";
        return false;
    }
    store_ = next;
    if (new_mac) *new_mac = mac;
    return true;
}

}  // namespace identity_store
