#include "strip_stats.hpp"

#if __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#include <cJSON.h>  // ESP-IDF "json" component
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

namespace strip_stats {

using color_pipeline::RGB8;

std::string local_time(double epoch_s) {
    if (!(epoch_s >= kMinValidEpoch)) return "";
    std::time_t tt = static_cast<std::time_t>(epoch_s);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

std::string local_date(double epoch_s) {
    const std::string t = local_time(epoch_s);
    return t.size() >= 10 ? t.substr(0, 10) : std::string();
}

namespace {

void ensure_size(std::vector<LedTotals>& v, size_t n) {
    if (v.size() < n) v.resize(n);
}
void ensure_size(std::vector<double>& v, size_t n) {
    if (v.size() < n) v.resize(n, 0.0);
}

double num(const cJSON* o, const char* k, double fb = 0.0) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(it) && std::isfinite(it->valuedouble) ? it->valuedouble : fb;
}
std::string str(const cJSON* o, const char* k) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(it) && it->valuestring ? it->valuestring : "";
}
std::vector<double> nums(const cJSON* o, const char* k) {
    std::vector<double> out;
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(o, k);
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr) out.push_back(cJSON_IsNumber(it) && std::isfinite(it->valuedouble) ? it->valuedouble : 0.0);
    return out;
}

// ---- v0.41: JSON as text (no cJSON tree) --------------------------------------------
struct Json {
    std::string s;
    bool first = true;
    void sep() {
        if (!first) s += ',';
        first = false;
    }
    void raw_num(double x, double round_to) {
        if (!std::isfinite(x)) x = 0.0;
        if (round_to > 0.0) x = std::round(x / round_to) * round_to;
        char b[32];
        std::snprintf(b, sizeof(b), "%.12g", x);
        s += b;
    }
    void raw_str(const std::string& v) {
        s += '"';
        for (char c : v) {
            if (c == '"' || c == '\\') {
                s += '\\';
                s += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                s += ' ';
            } else {
                s += c;
            }
        }
        s += '"';
    }
    void key(const char* k) {
        sep();
        s += '"';
        s += k;
        s += "\":";
    }
    void num(const char* k, double x, double round_to = 0.0) {
        key(k);
        raw_num(x, round_to);
    }
    void boolean(const char* k, bool v) {
        key(k);
        s += v ? "true" : "false";
    }
    void string(const char* k, const std::string& v) {
        key(k);
        raw_str(v);
    }
    template <class T, class F>
    void array(const char* k, const T& items, F each) {
        key(k);
        s += '[';
        bool f = true;
        for (const auto& x : items) {
            if (!f) s += ',';
            f = false;
            each(x);
        }
        s += ']';
    }
    void nums(const char* k, const std::vector<double>& v, double round_to) {
        array(k, v, [&](double x) { raw_num(x, round_to); });
    }
    void open(const char* k) {  // k = nullptr: an array element (array() writes the commas)
        if (k) key(k);
        s += '{';
        first = true;
    }
    void close() {
        s += '}';
        first = false;
    }
};

void totals_text(Json& j, const char* k, const Totals& t) {
    j.open(k);
    j.string("since", t.since);
    j.num("stream_s", t.stream_s, 0.1);
    j.num("over_s", t.over_s, 0.1);
    j.num("frames", static_cast<double>(t.frames));
    j.num("frames_limited", static_cast<double>(t.frames_limited));
    j.num("max_wanted_ma", t.max_wanted_ma, 1.0);
    std::vector<double> v;
    v.reserve(t.leds.size());
    for (const auto& l : t.leds) v.push_back(l.mah);
    j.nums("mah", v, 0.0001);
    v.clear();
    for (const auto& l : t.leds) v.push_back(l.s_hot);
    j.nums("s_hot", v, 0.1);
    v.clear();
    for (const auto& l : t.leds) v.push_back(l.s_peak);
    j.nums("s_peak", v, 0.1);
    v.clear();
    for (const auto& l : t.leds) v.push_back(t.stream_s > 0 ? l.level_s / t.stream_s : 0.0);
    j.nums("avg_level", v, 0.0001);
    j.close();
}

void episode_text(Json& j, const Episode& e) {
    j.open(nullptr);
    j.string("start", e.start);
    j.num("start_epoch", e.start_epoch);
    j.num("last_epoch", e.last_epoch);
    j.num("duration_s", e.duration_s, 0.1);
    j.num("frames", static_cast<double>(e.frames));
    j.string("worst_at", e.worst_at);
    j.num("wanted_ma", e.wanted_ma, 1.0);
    j.num("sent_ma", e.sent_ma, 1.0);
    j.string("mode", e.mode);
    j.array("peak_leds", e.peak_leds, [&](int p) { j.raw_num(p, 0.0); });
    j.num("peak_scale", e.peak_scale, 0.001);
    j.num("min_scale", e.min_scale, 0.001);
    j.num("min_scale_episode", e.min_scale_episode, 0.001);
    j.num("drift_min", e.drift_min, 0.01);
    j.num("drift_max", e.drift_max, 0.01);
    j.boolean("open", e.open);
    j.close();
}

Totals totals_from(const cJSON* o) {
    Totals t;
    if (!cJSON_IsObject(o)) return t;
    t.since = str(o, "since");
    t.stream_s = std::max(0.0, num(o, "stream_s"));
    t.over_s = std::max(0.0, num(o, "over_s"));
    t.frames = static_cast<uint64_t>(std::max(0.0, num(o, "frames")));
    t.frames_limited = static_cast<uint64_t>(std::max(0.0, num(o, "frames_limited")));
    t.max_wanted_ma = std::max(0.0, num(o, "max_wanted_ma"));
    auto mah = nums(o, "mah"), hot = nums(o, "s_hot"), peak = nums(o, "s_peak"), avg = nums(o, "avg_level");
    size_t n = std::min<size_t>(1000, std::max({mah.size(), hot.size(), peak.size(), avg.size()}));
    t.leds.resize(n);
    for (size_t i = 0; i < n; ++i) {
        if (i < mah.size()) t.leds[i].mah = mah[i];
        if (i < hot.size()) t.leds[i].s_hot = hot[i];
        if (i < peak.size()) t.leds[i].s_peak = peak[i];
        if (i < avg.size()) t.leds[i].level_s = avg[i] * t.stream_s;
    }
    return t;
}

Episode episode_from(const cJSON* o) {
    Episode e;
    e.start = str(o, "start");
    e.start_epoch = num(o, "start_epoch");
    e.last_epoch = num(o, "last_epoch", e.start_epoch);
    e.duration_s = num(o, "duration_s");
    e.frames = static_cast<uint64_t>(std::max(0.0, num(o, "frames")));
    e.worst_at = str(o, "worst_at");
    e.wanted_ma = num(o, "wanted_ma");
    e.sent_ma = num(o, "sent_ma");
    e.mode = str(o, "mode");
    for (double p : nums(o, "peak_leds")) e.peak_leds.push_back(static_cast<int>(p));
    e.peak_scale = num(o, "peak_scale", 1.0);
    e.min_scale = num(o, "min_scale", 1.0);
    e.min_scale_episode = num(o, "min_scale_episode", e.min_scale);
    e.drift_min = num(o, "drift_min");
    e.drift_max = num(o, "drift_max");
    e.open = false;  // an episode never stays open across a restart
    return e;
}

Storage file_storage(const std::string& path) {
    Storage st;
    if (path.empty()) return st;
    st.load = [path]() -> std::string {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return "";
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0 || size > (8 << 20)) {
            std::fclose(f);
            return "";
        }
        std::string buf(static_cast<size_t>(size), '\0');
        size_t got = std::fread(&buf[0], 1, buf.size(), f);
        std::fclose(f);
        return got == buf.size() ? buf : std::string();
    };
    st.save = [path](const std::string& text) {
        const std::string tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        bool ok = false;
        if (f) {
            ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
            ok = (std::fclose(f) == 0) && ok;
            if (ok) ok = std::rename(tmp.c_str(), path.c_str()) == 0;
        }
        if (!ok) std::fprintf(stderr, "[stats] could not write %s\n", path.c_str());
        return ok;
    };
    return st;
}

}  // namespace

Stats::Stats(std::string path) : storage_(file_storage(path)) { load(); }
Stats::Stats(Storage storage) : storage_(std::move(storage)) { load(); }

void Stats::load() {
    if (!storage_.load) return;
    const std::string buf = storage_.load();
    if (buf.empty()) return;
    cJSON* root = cJSON_ParseWithLength(buf.c_str(), buf.size());
    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        std::fprintf(stderr, "[stats] the saved log is not valid JSON - starting empty (overwritten at the next save)\n");
        return;
    }
    all_ = totals_from(cJSON_GetObjectItemCaseSensitive(root, "all_time"));
    since_ = totals_from(cJSON_GetObjectItemCaseSensitive(root, "since_reset"));
    install_ = totals_from(cJSON_GetObjectItemCaseSensitive(root, "since_install"));
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "days")) {
        Day d;
        d.date = str(it, "date");
        if (d.date.size() != 10) continue;
        if (kDayPerLed) {
            d.mah = nums(it, "mah");
            d.s_peak = nums(it, "s_peak");
        }
        double sum = 0;
        for (double x : nums(it, "mah")) sum += x;
        d.mah_total = num(it, "mah_total", sum);
        d.stream_s = num(it, "stream_s");
        d.over_s = num(it, "over_s");
        d.episodes = static_cast<int>(num(it, "episodes"));
        days_.push_back(std::move(d));
    }
    while (days_.size() > kMaxDays) days_.pop_front();
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "episodes")) {
        if (eps_.size() >= kMaxEpisodes) break;
        eps_.push_back(episode_from(it));
    }
    cJSON_Delete(root);
    std::fprintf(stderr, "[stats] loaded: %.1f h streamed in total, %u day(s), %u episode(s)%s\n", all_.stream_s / 3600.0,
                 static_cast<unsigned>(days_.size()), static_cast<unsigned>(eps_.size()),
                 install_.since.empty() ? ", strip not marked as mounted" : "");
}

void Stats::close_stale_episode_locked(double t) {
    if (!eps_.empty() && eps_.front().open && t - eps_.front().last_epoch > kEpisodeGapS) eps_.front().open = false;
}

void Stats::add_frame(const std::vector<RGB8>& out, const color_pipeline::Settings& s,
                      const color_pipeline::RenderInfo& info, double epoch_s, double dt_s, bool save) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool clock_ok = epoch_s >= kMinValidEpoch;
    // episodes need a time that moves: the wall clock, or (no clock) the frames' own time
    clock_ = clock_ok ? epoch_s : clock_ + std::max(0.0, std::min(dt_s, 3600.0));
    const double t = clock_;
    const std::string now_text = local_time(epoch_s);
    if (all_.since.empty()) all_.since = now_text;
    if (since_.since.empty()) since_.since = now_text;
    const bool installed = !install_.since.empty();
    const size_t n = out.size();
    ensure_size(all_.leds, n);
    ensure_size(since_.leds, n);
    if (installed) ensure_size(install_.leds, n);

    Day* day = nullptr;
    if (clock_ok) {
        const std::string date = now_text.substr(0, 10);
        if (days_.empty() || days_.back().date != date) {
            Day d;
            d.date = date;
            days_.push_back(std::move(d));
            while (days_.size() > kMaxDays) days_.pop_front();
        }
        day = &days_.back();
        if (kDayPerLed) {
            ensure_size(day->mah, n);
            ensure_size(day->s_peak, n);
        }
    }

    const bool limited = std::string(info.limit_mode) != "none";
    const double dt = (dt_s > 0.0 && dt_s <= kMaxFrameDtS) ? dt_s : 0.0;  // a pause is not streaming time
    const double sent_ma = color_pipeline::estimate_current_ma(out, s);

    Totals* sets[3] = {&all_, &since_, installed ? &install_ : nullptr};
    for (Totals* tt : sets) {
        if (!tt) continue;
        tt->frames++;
        if (limited) tt->frames_limited++;
        tt->stream_s += dt;
        if (limited) tt->over_s += dt;
        tt->max_wanted_ma = std::max(tt->max_wanted_ma, info.wanted_ma);
    }
    if (day) {
        day->stream_s += dt;
        if (limited) day->over_s += dt;
    }

    const bool peaks_mode = std::string(info.limit_mode) == "peaks";
    for (size_t i = 0; i < n; ++i) {
        const RGB8& c = out[i];
        const double level = (double(c.r) + double(c.g) + double(c.b)) / (3.0 * 255.0);
        const double ma = color_pipeline::led_current_ma(c, s);  // v0.33: the limiter's own model
        const double mah = ma * dt / 3600.0;
        const bool hot = std::max({c.r, c.g, c.b}) >= kHotLevel * 255.0;
        const bool peak = peaks_mode && std::find(info.peak_leds.begin(), info.peak_leds.end(), int(i)) != info.peak_leds.end();
        for (Totals* tt : sets) {
            if (!tt) continue;
            LedTotals& l = tt->leds[i];
            l.mah += mah;
            l.level_s += level * dt;
            if (hot) l.s_hot += dt;
            if (peak) l.s_peak += dt;
        }
        if (day) {
            day->mah_total += mah;
            if (kDayPerLed) {
                day->mah[i] += mah;
                if (peak) day->s_peak[i] += dt;
            }
        }
    }

    // Episodes: one per stretch of limited frames.
    close_stale_episode_locked(t);
    if (limited) {
        if (eps_.empty() || !eps_.front().open) {
            Episode e;
            e.start = now_text;
            e.start_epoch = e.last_epoch = t;
            e.drift_min = e.drift_max = info.peak_offset_leds;
            eps_.push_front(std::move(e));
            while (eps_.size() > kMaxEpisodes) eps_.pop_back();
            if (day) day->episodes++;
        }
        Episode& e = eps_.front();
        e.last_epoch = t;
        e.duration_s += dt;
        e.frames++;
        e.min_scale_episode = std::min(e.min_scale_episode, info.min_scale);
        e.drift_min = std::min(e.drift_min, info.peak_offset_leds);
        e.drift_max = std::max(e.drift_max, info.peak_offset_leds);
        if (info.wanted_ma >= e.wanted_ma) {
            e.wanted_ma = info.wanted_ma;
            e.sent_ma = sent_ma;
            e.worst_at = now_text;
            e.mode = info.limit_mode;
            e.peak_leds = info.peak_leds;
            e.peak_scale = info.peak_scale;
            e.min_scale = info.min_scale;
        }
    }
    dirty_ = true;

    if (save && t - last_save_epoch_ >= kSaveEveryS) {
        if (last_save_epoch_ > 0) save_locked();
        last_save_epoch_ = t;
    }
}

bool Stats::save_if_due(double epoch_s) {
    std::lock_guard<std::mutex> lk(mu_);
    const double t = epoch_s >= kMinValidEpoch ? epoch_s : clock_;
    if (!dirty_) return false;
    if (last_save_epoch_ == 0 || t < last_save_epoch_) {  // first call / the clock was set meanwhile
        last_save_epoch_ = t;
        return false;
    }
    if (t - last_save_epoch_ < kSaveEveryS) return false;
    last_save_epoch_ = t;
    return save_locked();
}

void Stats::reset_since(double epoch_s) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = since_.leds.size();
    since_ = Totals{};
    since_.since = local_time(epoch_s);
    since_.leds.resize(n);
    save_locked();
}

void Stats::reset_install(double epoch_s) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = std::max(install_.leds.size(), all_.leds.size());
    install_ = Totals{};
    install_.since = local_time(epoch_s);
    if (install_.since.empty()) install_.since = "(χωρίς ώρα)";  // marked, but the device does not know the time
    install_.leds.resize(n);
    save_locked();
}

bool Stats::save() {
    std::lock_guard<std::mutex> lk(mu_);
    return save_locked();
}

bool Stats::save_locked() {
    if (!storage_.save) return true;
    // the file: the same text without the page-only parts
    Json j;
    j.s.reserve(4096);
    j.s += '{';
    j.num("version", 2);
    totals_text(j, "all_time", all_);
    totals_text(j, "since_reset", since_);
    totals_text(j, "since_install", install_);
    j.array("days", days_, [&](const Day& d) {
        j.open(nullptr);
        j.string("date", d.date);
        j.num("stream_s", d.stream_s, 0.1);
        j.num("over_s", d.over_s, 0.1);
        j.num("episodes", d.episodes);
        j.num("mah_total", d.mah_total, 0.0001);
        if (kDayPerLed) {
            j.nums("mah", d.mah, 0.0001);
            j.nums("s_peak", d.s_peak, 0.1);
        }
        j.close();
    });
    j.array("episodes", eps_, [&](const Episode& e) { episode_text(j, e); });
    j.s += '}';
    const bool ok = storage_.save(j.s);
    if (ok) dirty_ = false;
    return ok;
}

std::string Stats::to_json_text(double epoch_s, bool for_file) const {
    std::lock_guard<std::mutex> lk(mu_);
    Json j;
    j.s.reserve(6144);
    j.s += '{';
    j.boolean("available", true);
    j.string("now", local_time(epoch_s));
    j.boolean("clock_ok", epoch_s >= kMinValidEpoch);
    j.num("hot_level", kHotLevel);
    j.num("episode_gap_s", kEpisodeGapS);
    totals_text(j, "all_time", all_);
    totals_text(j, "since_reset", since_);
    totals_text(j, "since_install", install_);
    j.boolean("installed", !install_.since.empty());
    j.array("days", days_, [&](const Day& d) {
        j.open(nullptr);
        j.string("date", d.date);
        j.num("stream_s", d.stream_s, 0.1);
        j.num("over_s", d.over_s, 0.1);
        j.num("episodes", d.episodes);
        j.num("mah_total", d.mah_total, 0.01);
        if (kDayPerLed && !for_file) {
            j.nums("mah", d.mah, 0.0001);
            j.nums("s_peak", d.s_peak, 0.1);
        }
        j.close();
    });
    const double t = epoch_s >= kMinValidEpoch ? epoch_s : clock_;
    j.array("episodes", eps_, [&](const Episode& e) {
        if (e.open && t - e.last_epoch > kEpisodeGapS) {
            Episode copy = e;
            copy.open = false;
            episode_text(j, copy);
        } else {
            episode_text(j, e);
        }
    });
    // v0.33 / v0.41: what the wear-aware limiter (setting wear_strength) sees: 0 = among the
    // least-used LEDs, 1 = kWearFullSpread of the mean (or more) above them. From the mAh
    // SINCE THE STRIP WAS MOUNTED (since_install), smoothed over 5 LEDs; empty until marked.
    {
        std::vector<double> mah;
        for (const auto& l : install_.leds) mah.push_back(l.mah);
        const std::vector<double> wear =
            install_.since.empty() ? std::vector<double>() : color_pipeline::wear_from_mah(mah, static_cast<int>(mah.size()));
        j.nums("wear", wear, 0.001);
    }
    j.string("wear_note",
             "wear = mAh per LED since the strip was marked as mounted, relative to the least-used LEDs (0..1); "
             "empty until «Η ταινία τοποθετήθηκε» is pressed");
    j.string("note", "estimates from the limiter's current model (mA per LED setting), not measurements");
    j.s += '}';
    return j.s;
}

cJSON* Stats::to_json(double epoch_s) const {
    cJSON* root = cJSON_Parse(to_json_text(epoch_s).c_str());
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "available", false);
    }
    return root;
}

std::vector<double> Stats::all_time_mah() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<double> out;
    out.reserve(all_.leds.size());
    for (const auto& l : all_.leds) out.push_back(l.mah);
    return out;
}
std::vector<double> Stats::install_mah() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<double> out;
    if (install_.since.empty()) return out;
    out.reserve(install_.leds.size());
    for (const auto& l : install_.leds) out.push_back(l.mah);
    return out;
}
Totals Stats::all_time() const {
    std::lock_guard<std::mutex> lk(mu_);
    return all_;
}
Totals Stats::since_reset() const {
    std::lock_guard<std::mutex> lk(mu_);
    return since_;
}
Totals Stats::since_install() const {
    std::lock_guard<std::mutex> lk(mu_);
    return install_;
}
std::vector<Episode> Stats::episodes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {eps_.begin(), eps_.end()};
}
std::vector<Day> Stats::days() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {days_.begin(), days_.end()};
}

}  // namespace strip_stats
