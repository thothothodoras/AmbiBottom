// Runtime-adjustable strip settings, persisted as JSON (cJSON - the same
// library already bundled as an ESP-IDF component, so this persistence
// layer ports to the real firmware with minimal change: only the storage
// backend changes (plain file here -> NVS there), not the JSON shape).
#pragma once
#include <mutex>
#include <string>
#include <vector>
#include "color_pipeline.hpp"

struct cJSON;

namespace settings {

// v0.15 (Phase 3): one set of limits for every way a setting can arrive
// (settings.json, PUT /ambi/settings, the monitor page's form). Values
// out of range are clamped, non-finite ones fall back to the default.
// Limits are documented in README "How the bridge limits the strip's current".
void sanitize(color_pipeline::Settings& s);

// The settings as a JSON object (caller owns it), same keys as settings.json.
cJSON* to_json(const color_pipeline::Settings& s);

// Partial merge: every known key present in `obj` with the right JSON type
// replaces the value in `s`; unknown keys and wrong types are ignored.
// Calls sanitize() at the end.
void merge_json(color_pipeline::Settings& s, const cJSON* obj);

// v0.32: strict check of a settings object BEFORE merge_json(), for the
// settings page: merge_json() quietly clamps and skips, which is right for a
// settings.json edited by hand but hides typing mistakes on a form. Returns
// one {field, message} per problem (wrong type, out of range, unknown name);
// empty = fine. Checks only what is in `obj` (a partial change is fine).
struct ValidationError {
    std::string field, message;
};
std::vector<ValidationError> validate_json(const cJSON* obj);

// Loads from `path`; on any error (missing file, bad JSON, out-of-range
// values) returns color_pipeline::Settings{} defaults and does NOT throw -
// a corrupt settings file must never crash the bridge.
color_pipeline::Settings load(const std::string& path);

// Writes `s` to `path` as pretty JSON. Returns false on I/O error (caller
// should log and continue with the in-memory value; settings persistence
// is best-effort, never load-bearing for the bridge to keep running).
bool save(const color_pipeline::Settings& s, const std::string& path);

// Thread-safe holder: the frame-handling thread reads `get()` on every
// HueStream frame, and the REST server's PUT /ambi/settings handler
// writes via `update()` - this is the runtime-settable config the user
// asked for ("config should live on the monitor's web page", not be a
// compile-time LED_COUNT constant). Every update() persists to `path`
// immediately while holding the lock, so a frame read right after a PUT
// never sees a value that hasn't been saved yet.
class SharedSettings {
public:
    explicit SharedSettings(std::string path) : path_(std::move(path)) {
        value_ = load(path_);
    }

    color_pipeline::Settings get() const {
        std::lock_guard<std::mutex> lk(mu_);
        return value_;
    }

    // Returns false if the write to disk failed (the in-memory value is
    // still updated either way - see save()'s own best-effort contract).
    bool update(const color_pipeline::Settings& s) {
        std::lock_guard<std::mutex> lk(mu_);
        value_ = s;
        return save(value_, path_);
    }

private:
    mutable std::mutex mu_;
    std::string path_;
    color_pipeline::Settings value_;
};

}  // namespace settings
