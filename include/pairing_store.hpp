// Hue pairing state: username -> {devicetype, clientkey}. This is the C++
// equivalent of fakebridge.py's STATE["users"] dict + save_state().
//
// IMPORTANT: usernames and clientkeys generated/stored here are the actual
// secret Hue pairing credentials. This file only ever talks to the TV and
// to its own JSON file on disk - it must never be logged in full or printed
// anywhere a chat session could see it. Log lines elsewhere in this project
// truncate these values (first 8 chars + "...") for the same reason.
#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pairing_store {

struct PairedUser {
    std::string devicetype;
    std::string clientkey_hex;  // 32 hex chars = 16 raw bytes, or empty if never requested
};

class Store {
public:
    // Loads from `path` if it exists and is valid JSON; otherwise starts
    // empty (never crashes on a missing/corrupt file - same policy as
    // settings::load).
    static Store load(const std::string& path);

    // Atomic (temp file + rename) write, same pattern as settings::save.
    bool save(const std::string& path) const;

    // Mirrors fakebridge.py's _pair(): if a user with this devicetype
    // already exists, returns it (optionally adding a clientkey if one was
    // requested and is still missing); otherwise creates a brand-new user.
    // `changed` is set true if the store was modified (caller should save).
    std::pair<std::string, PairedUser> pair(const std::string& devicetype,
                                             bool generate_clientkey,
                                             bool& changed);

    std::optional<PairedUser> get(const std::string& username) const;

    // Raw PSK bytes for a username (hex-decoded clientkey), or empty if the
    // username is unknown or has no clientkey yet. Suitable to hand
    // straight to dtls_psk::PskLookup.
    std::vector<uint8_t> psk_for(const std::string& username) const;

    const std::unordered_map<std::string, PairedUser>& users() const { return users_; }

private:
    std::unordered_map<std::string, PairedUser> users_;
};

// Thread-safe wrapper: the REST server (pairing, on the HTTP thread) and
// the DTLS server (PSK lookup, on its own thread) both touch the same
// store concurrently. Every mutating call persists to `path` immediately
// while still holding the lock, so a reader on the other thread never sees
// a pair() that hasn't been saved yet.
class ThreadSafeStore {
public:
    explicit ThreadSafeStore(std::string path) : path_(std::move(path)) {
        store_ = Store::load(path_);
    }

    std::pair<std::string, PairedUser> pair(const std::string& devicetype, bool generate_clientkey) {
        std::lock_guard<std::mutex> lk(mu_);
        bool changed = false;
        auto result = store_.pair(devicetype, generate_clientkey, changed);
        if (changed) store_.save(path_);
        last_paired_ = std::chrono::steady_clock::now();
        return result;
    }

    std::optional<PairedUser> get(const std::string& username) const {
        std::lock_guard<std::mutex> lk(mu_);
        return store_.get(username);
    }

    std::vector<uint8_t> psk_for(const std::string& username) const {
        std::lock_guard<std::mutex> lk(mu_);
        return store_.psk_for(username);
    }

    size_t user_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return store_.users().size();
    }

    // (username, devicetype) for every paired user - for the full config's
    // "whitelist", which a real bridge (and the old Python bridge) always
    // returns. No clientkeys here.
    std::vector<std::pair<std::string, std::string>> users_with_devicetype() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [name, u] : store_.users()) out.emplace_back(name, u.devicetype);
        return out;
    }

    // Virtual "link button" - the equivalent of the physical button on a
    // real Hue bridge (v9). Pressing it (monitor page "Pair" button, or
    // 'p' in the terminal dashboard) opens a pairing window; while it is
    // open POST /api is accepted, otherwise hue_rest answers error 101
    // "link button not pressed", exactly like a real bridge. Only gates
    // new pairing requests - already-paired users keep working as before.
    void press_link_button(std::chrono::seconds window) {
        std::lock_guard<std::mutex> lk(mu_);
        link_until_ = std::chrono::steady_clock::now() + window;
    }

    // Milliseconds left in the pairing window, 0 when closed.
    long link_remaining_ms() const {
        std::lock_guard<std::mutex> lk(mu_);
        auto now = std::chrono::steady_clock::now();
        if (link_until_ <= now) return 0;
        return static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(link_until_ - now).count());
    }

    // v0.15: "the TV wants to pair" prompt. hue_rest calls this when it
    // answers POST /api with 101 because the link button wasn't pressed;
    // the monitor page and the terminal footer then ask the user to press
    // Pair. Only refusals from the TV itself count (see hue_rest).
    void note_pair_refused() {
        std::lock_guard<std::mutex> lk(mu_);
        last_refused_ = std::chrono::steady_clock::now();
    }

    struct PairRequestState {
        long refused_ago_ms = -1;  // ms since the last refused POST /api, -1 = never
        long paired_ago_ms = -1;   // ms since the last successful pairing, -1 = never
        // true for `window` after a refusal, unless a pairing succeeded since.
        bool tv_waiting = false;
    };
    PairRequestState pair_request_state(std::chrono::milliseconds window = std::chrono::seconds(30)) const {
        std::lock_guard<std::mutex> lk(mu_);
        using namespace std::chrono;
        auto now = steady_clock::now();
        PairRequestState st;
        auto ago = [&](steady_clock::time_point t) {
            return t == steady_clock::time_point{} ? -1L
                                                   : static_cast<long>(duration_cast<milliseconds>(now - t).count());
        };
        st.refused_ago_ms = ago(last_refused_);
        st.paired_ago_ms = ago(last_paired_);
        st.tv_waiting = st.refused_ago_ms >= 0 && st.refused_ago_ms < window.count() &&
                        !(last_paired_ > last_refused_);
        return st;
    }

private:
    mutable std::mutex mu_;
    std::string path_;
    Store store_;
    std::chrono::steady_clock::time_point link_until_{};  // epoch = closed
    std::chrono::steady_clock::time_point last_refused_{}, last_paired_{};  // epoch = never
};

// secrets.token_hex(16) equivalent: 32 lowercase hex chars from
// /dev/urandom. Used for both the username and the clientkey, matching
// fakebridge.py exactly (clientkey is later uppercased there; we keep our
// own stored copy lowercase and uppercase only when handing it to the TV,
// since hex case doesn't matter for the actual byte value).
std::string random_hex_token(size_t num_bytes);

std::vector<uint8_t> hex_decode(const std::string& hex);
std::string hex_encode(const std::vector<uint8_t>& bytes);

}  // namespace pairing_store
