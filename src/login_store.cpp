#include "login_store.hpp"

#include <cjson/cJSON.h>
#include <fcntl.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace login_store {

std::string to_hex(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

namespace {

std::vector<unsigned char> from_hex(const std::string& h) {
    std::vector<unsigned char> out;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    if (h.size() % 2) return out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        int a = v(h[i]), b = v(h[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back(static_cast<unsigned char>(a * 16 + b));
    }
    return out;
}

bool same(const std::string& a, const std::string& b) {  // constant time for equal lengths
    if (a.size() != b.size()) return false;
    unsigned char x = 0;
    for (size_t i = 0; i < a.size(); ++i) x |= static_cast<unsigned char>(a[i] ^ b[i]);
    return x == 0;
}

bool write_private(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    fchmod(fd, 0600);
    size_t off = 0;
    while (off < text.size()) {
        ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

cJSON* read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return nullptr;
    std::stringstream ss;
    ss << f.rdbuf();
    return cJSON_Parse(ss.str().c_str());
}

std::string get_str(cJSON* o, const char* k) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

}  // namespace

std::string pbkdf2_hex(const std::string& password, const std::string& salt_hex, int iterations) {
    std::vector<unsigned char> salt = from_hex(salt_hex);
    unsigned char out[32];
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password.data()),
                                           password.size(), salt.data(), salt.size(),
                                           static_cast<unsigned int>(iterations), sizeof(out), out);
    return rc == 0 ? to_hex(out, sizeof(out)) : "";
}

std::string sha256_hex(const std::string& data) {
    unsigned char out[32];
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, 0) != 0) return "";
    return to_hex(out, sizeof(out));
}

bool random_bytes(unsigned char* out, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(out + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

std::string check_new_credentials(const std::string& user, const std::string& password) {
    if (user.empty() || user.size() > 32) return "username_1_to_32_chars";
    for (unsigned char c : user)
        if (c < 0x21 || c > 0x7e) return "username_ascii_no_spaces";
    if (password.size() < 8) return "password_min_8_chars";
    if (password.size() > 128) return "password_max_128_chars";
    return "";
}

Store::Store(std::string state_dir)
    : login_path_(state_dir + "/monitor_login.json"), sessions_path_(state_dir + "/monitor_sessions.json") {
    load();
}

void Store::load() {
    std::lock_guard<std::mutex> lk(mu_);
    if (cJSON* j = read_json(login_path_)) {
        user_ = get_str(j, "username");
        salt_hex_ = get_str(j, "salt");
        hash_hex_ = get_str(j, "hash");
        cJSON* it = cJSON_GetObjectItemCaseSensitive(j, "iterations");
        iterations_ = cJSON_IsNumber(it) && it->valueint >= 1000 ? it->valueint : kIterations;
        if (user_.empty() || salt_hex_.size() != 32 || hash_hex_.size() != 64) user_.clear();
        cJSON_Delete(j);
    }
    if (user_.empty()) return;  // no login set: old sessions mean nothing
    if (cJSON* j = read_json(sessions_path_)) {
        cJSON* arr = cJSON_GetObjectItemCaseSensitive(j, "sessions");
        cJSON* s;
        cJSON_ArrayForEach(s, arr) {
            std::string h = get_str(s, "token_sha256");
            cJSON* e = cJSON_GetObjectItemCaseSensitive(s, "expires");
            if (h.size() == 64 && cJSON_IsNumber(e) && sessions_.size() < static_cast<size_t>(kMaxSessions))
                sessions_[h] = static_cast<int64_t>(e->valuedouble);
        }
        cJSON_Delete(j);
    }
}

bool Store::configured() const {
    std::lock_guard<std::mutex> lk(mu_);
    return !user_.empty();
}

std::string Store::username() const {
    std::lock_guard<std::mutex> lk(mu_);
    return user_;
}

bool Store::save_login() const {
    cJSON* j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "username", user_.c_str());
    cJSON_AddStringToObject(j, "salt", salt_hex_.c_str());
    cJSON_AddStringToObject(j, "hash", hash_hex_.c_str());
    cJSON_AddNumberToObject(j, "iterations", iterations_);
    cJSON_AddStringToObject(j, "algorithm", "pbkdf2-hmac-sha256");
    char* t = cJSON_Print(j);
    bool ok = t && write_private(login_path_, std::string(t) + "\n");
    if (t) cJSON_free(t);
    cJSON_Delete(j);
    return ok;
}

void Store::save_sessions() const {
    cJSON* j = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(j, "sessions");
    for (const auto& kv : sessions_) {
        cJSON* s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "token_sha256", kv.first.c_str());
        cJSON_AddNumberToObject(s, "expires", static_cast<double>(kv.second));
        cJSON_AddItemToArray(arr, s);
    }
    char* t = cJSON_Print(j);
    if (t) write_private(sessions_path_, std::string(t) + "\n");
    if (t) cJSON_free(t);
    cJSON_Delete(j);
}

bool Store::set_credentials(const std::string& user, const std::string& password, std::string* err) {
    std::string e = check_new_credentials(user, password);
    if (!e.empty()) {
        if (err) *err = e;
        return false;
    }
    unsigned char salt[16];
    if (!random_bytes(salt, sizeof(salt))) {
        if (err) *err = "no_random";
        return false;
    }
    std::string salt_hex = to_hex(salt, sizeof(salt));
    std::string hash = pbkdf2_hex(password, salt_hex, kIterations);
    if (hash.empty()) {
        if (err) *err = "hash_failed";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    user_ = user;
    salt_hex_ = salt_hex;
    hash_hex_ = hash;
    iterations_ = kIterations;
    sessions_.clear();
    fails_.clear();
    save_sessions();
    if (!save_login()) {
        if (err) *err = "save_failed";
        return false;
    }
    return true;
}

bool Store::clear(std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    user_.clear();
    salt_hex_.clear();
    hash_hex_.clear();
    sessions_.clear();
    fails_.clear();
    bool ok = (::unlink(login_path_.c_str()) == 0 || errno == ENOENT);
    ::unlink(sessions_path_.c_str());
    if (!ok && err) *err = "save_failed";
    return ok;
}

void Store::prune(int64_t now_s) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second <= now_s) it = sessions_.erase(it);
        else ++it;
    }
}

std::string Store::login(const std::string& user, const std::string& password, const std::string& ip, int64_t now_s,
                         std::string* err) {
    std::string salt, want, name;
    int iters;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (user_.empty()) {
            if (err) *err = "not_configured";
            return "";
        }
        Fail& f = fails_[ip];
        if (f.locked_until > now_s) {
            if (err) *err = "locked:" + std::to_string(f.locked_until - now_s);
            return "";
        }
        salt = salt_hex_;
        want = hash_hex_;
        name = user_;
        iters = iterations_;
    }
    // Always hash (same time whether the name is right or not).
    std::string got = password.size() <= 128 ? pbkdf2_hex(password, salt, iters) : std::string();
    const bool ok = !got.empty() && same(got, want) && same(user, name);
    std::lock_guard<std::mutex> lk(mu_);
    if (user_ != name || hash_hex_ != want) {  // changed meanwhile
        if (err) *err = "bad_credentials";
        return "";
    }
    Fail& f = fails_[ip];
    if (!ok) {
        if (++f.count >= kMaxFailures) {
            f.count = 0;
            f.locked_until = now_s + kLockSeconds;
        }
        if (fails_.size() > 256) fails_.clear();  // bounded memory
        if (err) *err = "bad_credentials";
        return "";
    }
    fails_.erase(ip);
    unsigned char raw[32];
    if (!random_bytes(raw, sizeof(raw))) {
        if (err) *err = "no_random";
        return "";
    }
    std::string token = to_hex(raw, sizeof(raw));
    prune(now_s);
    while (sessions_.size() >= static_cast<size_t>(kMaxSessions)) {  // drop the one expiring first
        auto oldest = sessions_.begin();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
            if (it->second < oldest->second) oldest = it;
        sessions_.erase(oldest);
    }
    sessions_[sha256_hex(token)] = now_s + kSessionSeconds;
    save_sessions();
    return token;
}

bool Store::valid(const std::string& token, int64_t now_s) const {
    if (token.size() != 64) return false;
    std::string h = sha256_hex(token);
    std::lock_guard<std::mutex> lk(mu_);
    if (user_.empty()) return false;
    auto it = sessions_.find(h);
    return it != sessions_.end() && it->second > now_s;
}

void Store::logout(const std::string& token) {
    if (token.empty()) return;
    std::string h = sha256_hex(token);
    std::lock_guard<std::mutex> lk(mu_);
    if (sessions_.erase(h)) save_sessions();
}

}  // namespace login_store
