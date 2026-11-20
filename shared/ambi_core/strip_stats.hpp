// v0.23 (NEXT 6): a log of the strip output, per LED - not per frame
// (25 fps x 43 LEDs is too much), but running totals, so wear can be seen
// and the rolling peaks (NEXT 5) can be measured.
//
// Kept four ways:
//  - all_time:      never reset (real wear of this strip, and of everything before it)
//  - since_reset:   the page's reset button clears only these
//  - since_install: v0.41 (queue item 5): from the moment the user pressed «Η ταινία
//                   τοποθετήθηκε / αλλάχτηκε τώρα»; empty (not counted) until then.
//                   The wear-aware limiter (wear_strength) reads ONLY these, so streams
//                   with no strip on the wall, or before the strip was cut, do not count.
//  - days:          the last kMaxDays days: streaming / over-budget time, episode count,
//                   mAh per LED (on the ESP32 only the day's total, to save RAM)
// and a short history of the limiter's compromises: one EPISODE per stretch
// of over-budget frames (gaps < kEpisodeGapS merge), last kMaxEpisodes.
//
// All numbers come from the same current MODEL as the limiter
// (estimate_current_ma: idle + level x mA-per-LED-at-full-white), so they
// are estimates, not measurements, and only as good as ma_per_led_full_white.
//
// v0.41: shared by the Linux bridge (<state-dir>/strip_stats.json) and the ESP32
// (WLED's filesystem, through Storage). The JSON is written as text directly
// (to_json_text), without a cJSON tree: on the ESP32 a tree of ~1000 numbers
// would take ~60 KB of heap for a moment.
//
// Wall clock: epoch_s < kMinValidEpoch = "time not known yet" (an ESP32 without
// NTP): totals still count, days and the times of episodes do not.
// Thread-safe: add_frame() runs on the frame thread, the rest anywhere.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "color_pipeline.hpp"

struct cJSON;

namespace strip_stats {

constexpr double kHotLevel = 0.95;      // "at >= 95 %": brightest channel >= 95 % of 255
constexpr double kEpisodeGapS = 1.5;    // over-budget frames closer than this = one episode
constexpr double kMaxFrameDtS = 0.2;    // a longer gap between frames = stream pause, not counted
constexpr double kMinValidEpoch = 1.6e9;  // 2020-09: below = the clock is not set
#ifdef ESP_PLATFORM
constexpr size_t kMaxEpisodes = 20;
constexpr size_t kMaxDays = 14;
constexpr bool kDayPerLed = false;      // per-LED mAh per day: ~690 bytes a day on the heap
constexpr double kSaveEveryS = 300.0;   // flash writes: every 5 min while streaming
#else
constexpr size_t kMaxEpisodes = 50;
constexpr size_t kMaxDays = 30;
constexpr bool kDayPerLed = true;
constexpr double kSaveEveryS = 60.0;
#endif

struct LedTotals {
    double mah = 0;      // estimated mA-hours
    double s_hot = 0;    // seconds with the brightest channel >= 95 %
    double s_peak = 0;   // seconds held as a limiter peak while over budget
    double level_s = 0;  // integral of the level (0..1) over time -> average = level_s / stream_s
};

struct Totals {
    std::string since;            // local time these totals started ("" = unknown / not started)
    std::vector<LedTotals> leds;
    double stream_s = 0;          // time with frames arriving
    double over_s = 0;            // of which over the budget (limited)
    uint64_t frames = 0, frames_limited = 0;
    double max_wanted_ma = 0;     // highest current a frame asked for (before the limit)
};

struct Day {
    std::string date;             // YYYY-MM-DD, local
    std::vector<double> mah, s_peak;  // per LED (empty where kDayPerLed is false)
    double mah_total = 0;
    double stream_s = 0, over_s = 0;
    int episodes = 0;
};

struct Episode {
    std::string start;            // local time, "YYYY-MM-DD HH:MM:SS" ("" = clock not set)
    double start_epoch = 0, last_epoch = 0;
    double duration_s = 0;
    uint64_t frames = 0;
    // the worst frame of the episode (highest wanted current)
    std::string worst_at;
    double wanted_ma = 0;         // what the picture asked for
    double sent_ma = 0;           // what was sent instead (after limit + rise)
    std::string mode;             // limiter mode: peaks / anchors / uniform
    std::vector<int> peak_leds;   // peaks kept (LED index, 0-based)
    double peak_scale = 1.0;      // the peaks' level (1 = 100 %)
    double min_scale = 1.0;       // how far the rest was dimmed (lowest per-LED scale)
    double min_scale_episode = 1.0;  // lowest per-LED scale over the whole episode
    double drift_min = 0, drift_max = 0;  // range of the rolling-peak offset (LEDs)
    bool open = true;
};

// Where the log is kept. load: "" = nothing yet. save: true = written.
struct Storage {
    std::function<std::string()> load;
    std::function<bool(const std::string&)> save;
};

class Stats {
public:
    // A file (Linux): loads `path` if it exists (missing or corrupt -> empty,
    // never throws), saves through `path`.tmp + rename. Empty path = in memory only (tests).
    explicit Stats(std::string path);
    // v0.41: any storage (the ESP32: WLED's filesystem).
    explicit Stats(Storage storage);

    // One frame that went to the strip. `out` = what was sent (after the
    // rise limit), `info` = what render_strip did for its target,
    // `epoch_s` = wall clock (seconds since 1970, LOCAL on the ESP32 - see
    // local_time), `mono_s` = a steady clock (episode gaps when the wall clock
    // is not set), `dt_s` = time since the previous frame.
    // Saves at most every kSaveEveryS (save=false: never here; the caller calls save_if_due()).
    void add_frame(const std::vector<color_pipeline::RGB8>& out, const color_pipeline::Settings& s,
                   const color_pipeline::RenderInfo& info, double epoch_s, double dt_s, bool save = true);
    // v0.41: saves when kSaveEveryS have passed since the last save and something changed.
    bool save_if_due(double epoch_s);

    // Clears since_reset (all_time, since_install, days and episodes stay). Saves.
    void reset_since(double epoch_s);
    // v0.41: «the strip was mounted / replaced now»: clears since_install and starts it.
    // Saves. The wear-aware limiter reads only since_install.
    void reset_install(double epoch_s);

    bool save();
    // {"available":true,"now":..,"all_time":{...},"since_reset":{...},"since_install":{...},
    //  "installed":bool,"days":[...],"episodes":[...],"wear":[...],...} (caller owns)
    cJSON* to_json(double epoch_s) const;
    // the same as text (what to_json parses; the file is this with for_file = true)
    std::string to_json_text(double epoch_s, bool for_file = false) const;

    // for tests
    Totals all_time() const;
    // v0.33: only the per-LED all-time mAh.
    std::vector<double> all_time_mah() const;
    // v0.41: the per-LED mAh since the strip was mounted (empty = not marked yet):
    // what the wear-aware limiter uses.
    std::vector<double> install_mah() const;
    Totals since_reset() const;
    Totals since_install() const;
    std::vector<Episode> episodes() const;  // newest first
    std::vector<Day> days() const;          // oldest first

private:
    void close_stale_episode_locked(double t);
    bool save_locked();
    void load();

    mutable std::mutex mu_;
    Storage storage_;
    Totals all_, since_, install_;
    std::deque<Day> days_;
    std::deque<Episode> eps_;  // newest first
    double last_save_epoch_ = 0;
    bool dirty_ = false;
    double clock_ = 0;         // v0.41: the time episodes are measured in (epoch, or a steady count without a clock)
};

// Local time helpers (exposed for tests). "" when epoch_s < kMinValidEpoch.
std::string local_time(double epoch_s);  // YYYY-MM-DD HH:MM:SS
std::string local_date(double epoch_s);  // YYYY-MM-DD

}  // namespace strip_stats
