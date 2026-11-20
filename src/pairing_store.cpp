#include "pairing_store.hpp"

#include <cjson/cJSON.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace pairing_store {

namespace {
const char kHexChars[] = "0123456789abcdef";
}

std::string hex_encode(const std::vector<uint8_t>& bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kHexChars[b >> 4]);
        out.push_back(kHexChars[b & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> hex_decode(const std::string& hex) {
    std::vector<uint8_t> out;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    if (hex.size() % 2 != 0) return out;  // malformed -> empty, never half a byte
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};  // malformed -> empty, never garbage bytes
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string random_hex_token(size_t num_bytes) {
    std::vector<uint8_t> buf(num_bytes);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom) {
        throw std::runtime_error("cannot open /dev/urandom for pairing token generation");
    }
    urandom.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!urandom) {
        throw std::runtime_error("short read from /dev/urandom for pairing token generation");
    }
    return hex_encode(buf);
}

Store Store::load(const std::string& path) {
    Store s;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return s;  // no file yet -> empty store, not an error

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1 << 20) {  // sane upper bound; never trust a huge file
        std::fclose(f);
        return s;
    }
    std::string buf(static_cast<size_t>(size), '\0');
    size_t read = std::fread(&buf[0], 1, static_cast<size_t>(size), f);
    std::fclose(f);
    if (read != static_cast<size_t>(size)) return s;

    cJSON* root = cJSON_ParseWithLength(buf.c_str(), buf.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return s;  // malformed JSON -> empty store, never crash
    }

    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, root) {
        if (!entry->string || !cJSON_IsObject(entry)) continue;
        PairedUser u;
        cJSON* dt = cJSON_GetObjectItemCaseSensitive(entry, "devicetype");
        if (cJSON_IsString(dt) && dt->valuestring) u.devicetype = dt->valuestring;
        cJSON* ck = cJSON_GetObjectItemCaseSensitive(entry, "clientkey");
        if (cJSON_IsString(ck) && ck->valuestring) u.clientkey_hex = ck->valuestring;
        s.users_[entry->string] = std::move(u);
    }
    cJSON_Delete(root);
    return s;
}

bool Store::save(const std::string& path) const {
    cJSON* root = cJSON_CreateObject();
    for (const auto& [username, u] : users_) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "devicetype", u.devicetype.c_str());
        if (!u.clientkey_hex.empty()) {
            cJSON_AddStringToObject(entry, "clientkey", u.clientkey_hex.c_str());
        }
        cJSON_AddItemToObject(root, username.c_str(), entry);
    }

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;

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

std::pair<std::string, PairedUser> Store::pair(const std::string& devicetype,
                                                bool generate_clientkey,
                                                bool& changed) {
    changed = false;
    for (auto& [username, u] : users_) {
        if (u.devicetype == devicetype) {
            if (generate_clientkey && u.clientkey_hex.empty()) {
                u.clientkey_hex = random_hex_token(16);
                changed = true;
            }
            return {username, u};
        }
    }
    std::string username = random_hex_token(16);
    PairedUser u;
    u.devicetype = devicetype;
    if (generate_clientkey) {
        u.clientkey_hex = random_hex_token(16);
    }
    users_[username] = u;
    changed = true;
    return {username, u};
}

std::optional<PairedUser> Store::get(const std::string& username) const {
    auto it = users_.find(username);
    if (it == users_.end()) return std::nullopt;
    return it->second;
}

std::vector<uint8_t> Store::psk_for(const std::string& username) const {
    auto it = users_.find(username);
    if (it == users_.end() || it->second.clientkey_hex.empty()) return {};
    return hex_decode(it->second.clientkey_hex);
}

}  // namespace pairing_store
