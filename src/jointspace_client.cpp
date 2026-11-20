#include "jointspace_client.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

#include <cjson/cJSON.h>

#include <sys/stat.h>
#include <unistd.h>

namespace jointspace {

// v0.37: the client itself (TLS, Digest, parsing) is in shared/ambi_core/jointspace_fetch.cpp.

// ---------------------------------------------------------------------
// Credential storage - same atomic temp-file+rename pattern used
// throughout this project (pairing_store.cpp, settings.cpp, hue_rest.cpp's
// save_groups()), same validation and status() shape as the Python
// original's JointSpaceSettings.
// ---------------------------------------------------------------------

Settings::Settings(std::string tv_ip, std::string initial_user, std::string initial_pass, std::string path)
    : tv_ip_(std::move(tv_ip)), user_(std::move(initial_user)), pass_(std::move(initial_pass)), path_(std::move(path)) {
    std::ifstream f(path_, std::ios::binary);
    if (!f) return;  // no saved file yet - CLI/env credentials (if any) stand
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) {
        load_error_ = true;
        return;
    }
    cJSON* u = cJSON_GetObjectItemCaseSensitive(root, "username");
    cJSON* p = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(u) && cJSON_IsString(p)) {
        user_ = u->valuestring;
        pass_ = p->valuestring;
        persisted_ = true;
    } else {
        load_error_ = true;
    }
    cJSON_Delete(root);
}

Settings::Status Settings::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    Status st;
    st.available = !tv_ip_.empty();
    st.configured = !user_.empty() && !pass_.empty();
    st.persisted = persisted_;
    st.load_error = load_error_;
    return st;
}

namespace {
bool valid_credential_field(const std::string& s) {
    if (s.empty() || s.size() > 512) return false;
    for (char c : s)
        if (c == '\0' || c == '\r' || c == '\n') return false;
    return true;
}
}  // namespace

bool Settings::update(const std::string& username, const std::string& password, std::string& error_code) {
    if (!valid_credential_field(username) || !valid_credential_field(password)) {
        error_code = "invalid_credentials";
        return false;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "username", username.c_str());
    cJSON_AddStringToObject(root, "password", password.c_str());
    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        error_code = "storage_unavailable";
        return false;
    }
    std::string tmp = path_ + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        cJSON_free(text);
        error_code = "storage_unavailable";
        return false;
    }
    size_t len = std::strlen(text);
    bool ok = std::fwrite(text, 1, len, f) == len;
    std::fclose(f);
    cJSON_free(text);
    if (ok) ok = (::chmod(tmp.c_str(), 0600) == 0);
    if (ok) ok = (std::rename(tmp.c_str(), path_.c_str()) == 0);
    if (!ok) {
        std::remove(tmp.c_str());
        error_code = "storage_unavailable";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        user_ = username;
        pass_ = password;
        persisted_ = true;
        load_error_ = false;
        ++revision_;
    }
    cv_.notify_all();
    return true;
}

Settings::Capture Settings::capture() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {revision_, tv_ip_, user_, pass_};
}

void Settings::wait_for_change_or(int delay_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    uint64_t seen = revision_;
    cv_.wait_for(lk, std::chrono::milliseconds(delay_ms), [&] { return revision_ != seen; });
}

void run_poll_loop(Settings& settings, int interval_ms, const ResultCallback& on_result) {
    Client client;  // one kept-alive connection for the life of the loop
    std::string prev_ip, prev_user, prev_pass;
    int failures = 0;
    while (true) {
        auto cap = settings.capture();
        if (cap.tv_ip != prev_ip || cap.username != prev_user || cap.password != prev_pass) {
            prev_ip = cap.tv_ip;
            prev_user = cap.username;
            prev_pass = cap.password;
            failures = 0;
        }
        int delay = interval_ms;
        if (cap.tv_ip.empty() || cap.username.empty() || cap.password.empty()) {
            on_result(false, "not_configured", "", {}, {}, {});
        } else {
            Sides sides;
            std::string err;
            bool ok = client.fetch(cap.tv_ip, cap.username, cap.password, sides, err);
            if (ok) {
                failures = 0;
                on_result(true, "live", "", std::move(sides.left), std::move(sides.top), std::move(sides.right));
            } else {
                failures = std::min(failures + 1, 4);
                // Same backoff shape as the old bridge: don't hammer a TV
                // that's off, asleep, or rejecting credentials.
                int backoff_ms = static_cast<int>(std::min(5000.0, 500.0 * (1 << failures)));
                delay = std::max(delay, backoff_ms);
                on_result(false, "unavailable", err, {}, {}, {});
            }
        }
        settings.wait_for_change_or(delay);
    }
}

}  // namespace jointspace
