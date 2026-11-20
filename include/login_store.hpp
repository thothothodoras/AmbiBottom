// v0.28: one user + password for editing the monitor page from other
// devices (today only this Linux machine's own browser may change settings).
//
// - The password is stored as PBKDF2-HMAC-SHA256 (random 16-byte salt,
//   kIterations rounds) in <state-dir>/monitor_login.json, mode 0600. It is
//   set or removed only from the local browser (127.0.0.1).
// - A login gives a random 32-byte session token (cookie, HttpOnly,
//   SameSite=Strict). Only SHA-256(token) is kept, in memory and in
//   <state-dir>/monitor_sessions.json (0600), so a stop/start/restart of the
//   bridge (execv) does not log the user out.
// - Failed logins: after 5 in a row from one IP, that IP waits 60 s.
// - Plain HTTP: the password crosses the home LAN unencrypted.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace login_store {

constexpr int kIterations = 100000;
constexpr int64_t kSessionSeconds = 7 * 24 * 3600;
constexpr int kMaxSessions = 16;
constexpr int kMaxFailures = 5;
constexpr int64_t kLockSeconds = 60;

// Pure helpers (also used by the tests).
std::string to_hex(const unsigned char* p, size_t n);
std::string pbkdf2_hex(const std::string& password, const std::string& salt_hex, int iterations);
std::string sha256_hex(const std::string& data);
bool random_bytes(unsigned char* out, size_t n);
// "" when fine, else an error code for the page.
std::string check_new_credentials(const std::string& user, const std::string& password);

class Store {
public:
    explicit Store(std::string state_dir);

    bool configured() const;
    std::string username() const;

    // Local browser only (the caller checks). Removes every session.
    bool set_credentials(const std::string& user, const std::string& password, std::string* err);
    bool clear(std::string* err);

    // Returns the new session token ("" = refused; *err = bad_credentials |
    // locked:<seconds> | not_configured). now_s = wall clock seconds.
    std::string login(const std::string& user, const std::string& password, const std::string& ip, int64_t now_s,
                      std::string* err);
    bool valid(const std::string& token, int64_t now_s) const;
    void logout(const std::string& token);

private:
    void load();
    bool save_login() const;
    void save_sessions() const;
    void prune(int64_t now_s);

    std::string login_path_, sessions_path_;
    mutable std::mutex mu_;
    std::string user_, salt_hex_, hash_hex_;
    int iterations_ = kIterations;
    std::unordered_map<std::string, int64_t> sessions_;  // sha256(token) -> expiry
    struct Fail { int count = 0; int64_t locked_until = 0; };
    std::unordered_map<std::string, Fail> fails_;
};

}  // namespace login_store
