// "The TV locked itself to LightGroup after a DTLS failure" - the state behind
// the v0.31 banner, as plain logic with the clock passed in, so the Linux
// monitor and the ESP32 usermod use the same rules.
//
// Rules (from the TV's decompiled PostALState.LampUpdateThread, NOT measured
// on the TV - which is why the limit is a setting):
//  - stream.active=true starts a wait of `limit_s` (default 3 s);
//  - a good handshake ends the wait and clears a lock;
//  - a failed handshake while waiting, or the wait running out, sets the lock;
//  - the TV then creating / being answered a LightGroup marks it confirmed.
// C++17 standard library only.
#pragma once
#include <string>

namespace tv_lock {

struct Status {
    bool locked = false;
    std::string reason;   // "timeout" | "failed" | ""
    std::string detail;   // error text for "failed"
    double age_s = 0.0;
    bool confirmed = false;
    double last_handshake_latency_s = -1.0;  // stream.active=true -> handshake; -1 = none yet
};

class Tracker {
public:
    static constexpr double default_limit_s = 3.0;

    explicit Tracker(double limit_s = default_limit_s) : limit_s_(limit_s) {}

    double limit_s() const { return limit_s_; }
    void set_limit_s(double s) { limit_s_ = s > 0.1 ? s : 0.1; }

    // `now` is any monotonic clock in seconds; use the same one everywhere.
    void stream_requested(double now);
    void stream_released();
    // Returns the seconds from stream.active=true to this handshake when it
    // was ok and one was awaited, else -1.
    double handshake(bool ok, const std::string& detail, double now);
    // Call a few times per second. `last_frame_time`: when the last stream
    // frame was decrypted (same clock), negative = never. True exactly once
    // when the limit passed without a handshake.
    bool check_deadline(double now, double last_frame_time);
    void lightgroup_used();
    Status status(double now) const;

private:
    double limit_s_;
    bool waiting_ = false;
    double wait_since_ = 0.0;
    bool locked_ = false;
    bool confirmed_ = false;
    std::string reason_, detail_;
    double lock_since_ = 0.0;
    double last_latency_s_ = -1.0;
};

}  // namespace tv_lock
