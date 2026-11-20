#include "tv_lock.hpp"

namespace tv_lock {

void Tracker::stream_requested(double now) {
    waiting_ = true;
    wait_since_ = now;
}

void Tracker::stream_released() { waiting_ = false; }

double Tracker::handshake(bool ok, const std::string& detail, double now) {
    double latency = -1.0;
    if (ok) {
        if (waiting_) latency = now - wait_since_;
        waiting_ = false;
        locked_ = false;
        confirmed_ = false;
        if (latency >= 0) last_latency_s_ = latency;
    } else if (waiting_) {
        waiting_ = false;
        locked_ = true;
        reason_ = "failed";
        detail_ = detail;
        lock_since_ = now;
        confirmed_ = false;
    }
    return latency;
}

bool Tracker::check_deadline(double now, double last_frame_time) {
    if (!waiting_) return false;
    if (now - wait_since_ < limit_s_) return false;
    waiting_ = false;
    // Frames decrypted since the request = a session was already up (a client
    // that handshakes first and sets stream.active after).
    if (last_frame_time >= 0 && last_frame_time >= wait_since_) return false;
    locked_ = true;
    reason_ = "timeout";
    detail_.clear();
    lock_since_ = now;
    confirmed_ = false;
    return true;
}

void Tracker::lightgroup_used() {
    if (locked_) confirmed_ = true;
}

Status Tracker::status(double now) const {
    Status s;
    s.locked = locked_;
    s.reason = locked_ ? reason_ : "";
    s.detail = locked_ ? detail_ : "";
    s.age_s = locked_ ? now - lock_since_ : 0.0;
    s.confirmed = locked_ && confirmed_;
    s.last_handshake_latency_s = last_latency_s_;
    return s;
}

}  // namespace tv_lock
