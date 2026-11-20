#include "terminal_dashboard.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace terminal_dashboard {

namespace {

// EVENTS box minimum height (v9: was 2, which hid what the TV was doing).
// v0.17: the box grows to fill the window, and keeps kNoticeHistory lines
// to scroll back through.
constexpr size_t kNoticeLines = 5;
constexpr size_t kNoticeHistory = 500;

// Same idea as the Python original's safe_text(): anything that isn't a
// plain printable ASCII character becomes a space, so a stray control byte
// in, say, a TV-supplied header value can never corrupt the box layout or
// move the cursor somewhere unexpected. Multi-byte UTF-8 (Greek text in an
// error string, say) is left alone - only single control/DEL bytes are
// replaced.
std::string safe_text(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) c = ' ';
    }
    return out;
}

// v0.19: one log line -> rows of at most `width` columns, broken at a
// space when there is one in the second half of the row; continuation rows
// start with two spaces. ASCII only (safe_text has already run).
std::vector<std::string> wrap_line(const std::string& text, size_t width) {
    std::vector<std::string> rows;
    if (width < 8) {
        rows.push_back(text.substr(0, width));
        return rows;
    }
    std::string rest = text;
    bool first = true;
    while (true) {
        size_t w = first ? width : width - 2;
        std::string prefix = first ? "" : "  ";
        if (rest.size() <= w) {
            rows.push_back(prefix + rest);
            break;
        }
        size_t cut = rest.rfind(' ', w);
        if (cut == std::string::npos || cut < w / 2) cut = w;
        rows.push_back(prefix + rest.substr(0, cut));
        size_t next = rest.find_first_not_of(' ', cut);
        if (next == std::string::npos) break;
        rest = rest.substr(next);
        first = false;
    }
    return rows;
}

std::string pad_or_truncate(const std::string& s, size_t width) {
    std::string t = s.size() > width ? s.substr(0, width) : s;
    if (t.size() < width) t += std::string(width - t.size(), ' ');
    return t;
}

struct TermSize {
    size_t cols = 80, rows = 24;
};

TermSize terminal_size() {
    TermSize t;
    winsize ws{};
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 || ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) && ws.ws_col > 0) {
        t.cols = ws.ws_col;
        if (ws.ws_row > 0) t.rows = ws.ws_row;
    }
    return t;
}

// Box width follows the real terminal width (one column of slack), capped
// at 100 so it still reads as a dashboard on an ultra-wide window. Never
// wider than the terminal: until v7 there was also a 44-column *minimum*,
// so on a narrow window - or whenever the reported size was off - lines
// came out wider than the screen and wrapped, which is what turned the
// screen into the staircase of half-boxes seen on the real LXC. Autowrap
// is now also switched off while the dashboard is up (see start()), so
// even a wrong size can only clip a line at the right edge, never wrap it.
size_t box_width(const TermSize& t) {
    size_t w = t.cols > 1 ? t.cols - 1 : t.cols;
    return std::max<size_t>(24, std::min<size_t>(w, 100));
}

// Unicode box-drawing light horizontal (U+2500), repeated - matches the
// Python original's own box-drawing characters exactly, rather than mixing
// in plain ASCII hyphens for the fill (which look like a different line
// weight next to the "┌"/"┐"/"└"/"┘" corners).
std::string hbar(size_t n) {
    std::string s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) s += "\xe2\x94\x80";
    return s;
}

std::string box_top(const std::string& title, size_t width) {
    std::string t = " " + title + " ";
    // "┌─" + t + dashes + "┐" must total exactly `width` columns, same as
    // every other box row (v5/v6 used "- 2" here, one column too wide).
    size_t dashes = width > t.size() + 3 ? width - t.size() - 3 : 0;
    return "\xe2\x94\x8c\xe2\x94\x80" + t + hbar(dashes) + "\xe2\x94\x90";
}

std::string box_bottom(size_t width) {
    return "\xe2\x94\x94" + hbar(width - 2) + "\xe2\x94\x98";
}

// Plain-text row: content is safe_text'd, then padded/truncated by its own
// (correct, no-escape-codes) length. Used for every row except the three
// coloured lamp rows below.
std::string box_line(const std::string& content, size_t inner) {
    return "\xe2\x94\x82 " + pad_or_truncate(safe_text(content), inner) + " \xe2\x94\x82";
}

std::string age_text(double age_s) {
    if (age_s < 0) return "--";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f s", age_s);
    return buf;
}

std::string fps_text(double fps) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", fps);
    return buf;
}

// Same error-code -> human text mapping as the Python original's
// dashboard_lines(); jointspace_client.hpp documents these as the
// complete fixed set of codes fetch_processed_sides() can return.
std::string js_error_text(const std::string& code) {
    if (code == "authentication_failed") return "AUTH FAILED (401): check JointSpace pairing credentials";
    if (code == "forbidden") return "ACCESS DENIED (403)";
    if (code == "network_unavailable") return "TV unreachable";
    if (code == "network_timeout") return "TV request timed out";
    if (code == "invalid_response") return "Invalid TV colour data";
    if (code == "invalid_address") return "Invalid TV IP";
    if (code == "request_failed") return "TV request failed";
    return "";
}


// ---- v9 TV rectangle helpers ----------------------------------------------
struct Zone3 {
    bool valid = false;
    uint8_t r = 0, g = 0, b = 0;
};

// JointSpace "processed" values are 0..23 linear (native white = 23, see
// the page's Color.jointspace); the page then sRGB-encodes for display.
// Same here, so the terminal shows the same colours as the page.
Zone3 js_to_rgb(const monitor_state::Zone& z) {
    auto enc = [](uint8_t v) {
        double c = std::min(1.0, std::max(0.0, v / 23.0));
        double e = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
        return static_cast<uint8_t>(std::lround(255.0 * e));
    };
    Zone3 o;
    o.valid = true;
    o.r = enc(z.r);
    o.g = enc(z.g);
    o.b = enc(z.b);
    return o;
}

// A row of cells whose visible width is tracked separately from the string
// (which carries zero-width colour escapes).
struct Cells {
    std::string s;
    size_t vis = 0;
    void space(size_t n) {
        s += std::string(n, ' ');
        vis += n;
    }
    void swatch(const Zone3& z, size_t w) {
        char buf[48];
        if (z.valid)
            std::snprintf(buf, sizeof(buf), "\x1b[48;2;%d;%d;%dm", z.r, z.g, z.b);
        else
            std::snprintf(buf, sizeof(buf), "\x1b[48;2;48;62;81m");  // page's "no data" grey
        s += buf + std::string(w, ' ') + "\x1b[0m";
        vis += w;
    }
    // One row of the screen area: frame lines top/bottom, sides elsewhere,
    // an optional centred label.
    void screen(size_t w, const std::string& label, bool first, bool last) {
        if (w < 2) {
            space(w);
            return;
        }
        std::string fill;
        size_t inner_w = w - 2;
        if (first || last) {
            for (size_t i = 0; i < inner_w; ++i) fill += "\xe2\x94\x80";
            s += std::string("\x1b[2m") + (first ? "\xe2\x95\xad" : "\xe2\x95\xb0") + fill +
                 (first ? "\xe2\x95\xae" : "\xe2\x95\xaf") + "\x1b[0m";
        } else {
            std::string l = label.size() > inner_w ? label.substr(0, inner_w) : label;
            size_t lp = (inner_w - l.size()) / 2;
            s += "\x1b[2m\xe2\x94\x82\x1b[0m" + std::string(lp, ' ') + l +
                 std::string(inner_w - lp - l.size(), ' ') + "\x1b[2m\xe2\x94\x82\x1b[0m";
        }
        vis += w;
    }
    std::string boxed(size_t inner) const {
        std::string row = s;
        if (inner > vis) row += std::string(inner - vis, ' ');
        return "\xe2\x94\x82 " + row + " \xe2\x94\x82";
    }
};

}  // namespace

TerminalDashboard::TerminalDashboard(std::string monitor_url, std::string version)
    : monitor_url_(std::move(monitor_url)), version_(std::move(version)) {}

void TerminalDashboard::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (active_) return;
    // Alternate screen, hide cursor, autowrap OFF, clear. With autowrap
    // off, a line that is somehow still wider than the window gets clipped
    // at the right edge instead of wrapping onto the next row and pushing
    // every box below it out of place.
    const std::string seq = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J\x1b[H";
    std::fwrite(seq.data(), 1, seq.size(), stdout);
    std::fflush(stdout);
    active_ = true;
    last_cols_ = last_rows_ = 0;
}

void TerminalDashboard::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!active_) return;
    const std::string seq = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";  // autowrap back on
    std::fwrite(seq.data(), 1, seq.size(), stdout);
    std::fflush(stdout);
    active_ = false;
}

bool TerminalDashboard::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_;
}

void TerminalDashboard::notice(const std::string& line) {
    // Cap stored length defensively - this feeds from the redirected
    // stderr pipe, which could in principle carry one very long line.
    std::string trimmed = line.size() > 600 ? line.substr(0, 600) : line;
    std::lock_guard<std::mutex> lk(mu_);
    // v9: SSDP discovery probes (several per second from every device on
    // the LAN) would push everything useful out of a 5-line box - they
    // still go to ambi_bridge.log, just not here. A line identical to the
    // previous one (the TV polls lights/1 every 3 s while streaming) bumps
    // a repeat counter instead of taking another row.
    if (trimmed.rfind("[ssdp] M-SEARCH", 0) == 0) return;
    std::string clean = safe_text(trimmed);
    if (!notices_.empty() && notices_.back().first == clean) {
        ++notices_.back().second;
        return;
    }
    notices_.emplace_back(clean, 1);
    // Scrolled up: keep looking at the same rows while new lines arrive
    // (v0.19: a wrapped line pushes the view by all of its rows).
    if (scroll_ > 0) scroll_ += events_width_ ? wrap_line(clean, events_width_).size() : 1;
    while (notices_.size() > kNoticeHistory) notices_.pop_front();
}

void TerminalDashboard::toggle_tv() {
    std::lock_guard<std::mutex> lk(mu_);
    show_tv_ = !show_tv_;
    redraw_ = true;
}

void TerminalDashboard::scroll_log(int lines) {
    std::lock_guard<std::mutex> lk(mu_);
    long max_scroll =
        static_cast<long>(events_total_rows_ > events_rows_ ? events_total_rows_ - events_rows_ : 0);
    long s = static_cast<long>(scroll_) + lines;
    scroll_ = static_cast<size_t>(std::max(0L, std::min(max_scroll, s)));
    redraw_ = true;
}

void TerminalDashboard::scroll_log_pages(int pages) {
    size_t rows;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rows = std::max<size_t>(events_rows_, 1);
    }
    scroll_log(pages * static_cast<int>(rows > 1 ? rows - 1 : 1));
}

void TerminalDashboard::scroll_log_newest() {
    std::lock_guard<std::mutex> lk(mu_);
    scroll_ = 0;
    redraw_ = true;
}

void TerminalDashboard::render(const monitor_state::Snapshot& snap, bool stream_configured,
                               long link_remaining_ms, bool tv_waiting) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!active_) return;

    TermSize ts = terminal_size();
    size_t width = box_width(ts);
    size_t inner = width - 4;
    bool resized = ts.cols != last_cols_ || ts.rows != last_rows_;
    last_cols_ = ts.cols;
    last_rows_ = ts.rows;

    std::time_t t = std::time(nullptr);
    char clock_buf[16];
    std::strftime(clock_buf, sizeof(clock_buf), "%H:%M:%S", std::localtime(&t));

    std::vector<std::string> lines;
    char header[160];
    std::snprintf(header, sizeof(header), "AMBILIGHT BRIDGE  %s", version_.c_str());
    std::string header_line = header;
    size_t pad = width > header_line.size() + 8 ? width - header_line.size() - 8 : 1;
    lines.push_back(pad_or_truncate(header_line + std::string(pad, ' ') + clock_buf, width));
    lines.push_back(pad_or_truncate(safe_text("Monitor & credentials: " + monitor_url_), width));

    // ---- TV: the four Ambilight sides drawn as a rectangle (v9) ----------
    // Same picture as the monitor page: top zones along the top, the
    // JointSpace left/right zones down the sides (left reversed, like the
    // page, because JointSpace numbers the left side bottom-up), and the
    // three HueStream bottom lamps along the bottom. A grey cell = no live
    // data for that zone. Every piece is built with known visible widths
    // (Cells), never measured from the string, because colour escapes have
    // zero visible width.
    std::vector<Zone3> left, right, top, bottom;
    bool js_live = snap.js_status == "live";
    auto js_rgb = [](const monitor_state::Zone& z) { return js_to_rgb(z); };
    if (js_live) {
        for (auto it = snap.js_left.rbegin(); it != snap.js_left.rend(); ++it) left.push_back(js_rgb(*it));
        for (auto& z : snap.js_right) right.push_back(js_rgb(z));
        for (auto& z : snap.js_top) top.push_back(js_rgb(z));
    }
    if (left.empty()) left.assign(3, Zone3{});
    if (right.empty()) right.assign(3, Zone3{});
    if (top.empty()) top.assign(8, Zone3{});
    size_t nb = std::max<size_t>(snap.lamps.size(), 3);
    for (size_t i = 0; i < nb; ++i) {
        Zone3 z;
        if (snap.lamps_good && i < snap.lamps.size()) {
            z.valid = true;
            z.r = snap.lamps[i].rgb.r;
            z.g = snap.lamps[i].rgb.g;
            z.b = snap.lamps[i].rgb.b;
        }
        bottom.push_back(z);
    }

    // Geometry: side columns `sw` wide, a 1-column gap, then the screen /
    // top / bottom strips `strip` wide. Shrinks to fit narrow terminals.
    size_t sw = inner >= 50 ? 4 : 2;
    size_t nt = top.size();
    size_t tw = 2;
    for (size_t cand = 7; cand >= 2; --cand) {
        if (2 * (sw + 1) + nt * cand + (nt - 1) <= inner) { tw = cand; break; }
    }
    size_t strip = nt * tw + (nt - 1);
    size_t tv_w = 2 * (sw + 1) + strip;
    size_t margin = inner > tv_w ? (inner - tv_w) / 2 : 0;
    // Two text rows per side zone on a tall window, one on a short one, so
    // the whole dashboard still fits without its bottom being cut off.
    size_t zone_h = ts.rows >= 27 ? 2 : 1;
    size_t mid_rows = std::max(left.size(), right.size()) * zone_h;

    const size_t tv_first_line = lines.size();
    lines.push_back(box_top("TV  (top / left / right: JointSpace, bottom: Hue)", width));
    {   // top strip
        Cells c;
        c.space(margin + sw + 1);
        for (size_t i = 0; i < nt; ++i) {
            if (i) c.space(1);
            c.swatch(top[i], tw);
        }
        lines.push_back(c.boxed(inner));
    }
    for (size_t row = 0; row < mid_rows; ++row) {
        Cells c;
        c.space(margin);
        size_t li = row / zone_h * left.size() / std::max<size_t>(1, mid_rows / zone_h);
        size_t ri = row / zone_h * right.size() / std::max<size_t>(1, mid_rows / zone_h);
        c.swatch(li < left.size() ? left[li] : Zone3{}, sw);
        c.space(1);
        // the "screen": a thin frame so it reads as a TV, label in the middle
        std::string label;
        if (row == mid_rows / 2) label = snap.lamps_good ? "IMMERSIVE / LIVE" : "no Hue stream";
        c.screen(strip, label, row == 0, row + 1 == mid_rows);
        c.space(1);
        c.swatch(ri < right.size() ? right[ri] : Zone3{}, sw);
        lines.push_back(c.boxed(inner));
    }
    {   // bottom strip: the Hue lamps share the strip width
        Cells c;
        c.space(margin + sw + 1);
        size_t gaps = bottom.size() - 1;
        size_t avail = strip > gaps ? strip - gaps : bottom.size();
        for (size_t i = 0; i < bottom.size(); ++i) {
            if (i) c.space(1);
            size_t w = avail / bottom.size() + (i < avail % bottom.size() ? 1 : 0);
            c.swatch(bottom[i], w);
        }
        lines.push_back(c.boxed(inner));
    }
    lines.push_back(box_bottom(width));
    // v0.17: 't' hides the TV picture; the log takes its rows.
    if (!show_tv_) lines.erase(lines.begin() + static_cast<long>(tv_first_line), lines.end());

    // ---- STATUS: one line per source, plus the bottom lamps' numbers -----
    std::string js_status_text;
    if (!snap.js_error.empty()) js_status_text = js_error_text(snap.js_error);
    if (js_status_text.empty()) {
        if (snap.js_status == "not_configured") js_status_text = "NOT CONFIGURED";
        else if (snap.js_status == "live") js_status_text = "LIVE";
        else js_status_text = "WAITING";
    }
    std::string hue_status_text;
    if (!stream_configured) hue_status_text = "DISABLED";
    else if (snap.lamps_good) hue_status_text = "LIVE";
    else if (snap.old_session) hue_status_text = "OLD SESSION";
    else if (snap.tv_dtls_lock) hue_status_text = "TV LOCKED";
    else if (snap.stream_active) hue_status_text = (snap.hue_age_s >= 0) ? "STALE" : "WAITING";
    else hue_status_text = "INACTIVE";

    lines.push_back(box_top("STATUS", width));
    char js_meta[192];
    std::snprintf(js_meta, sizeof(js_meta), "Sides  %-8s %s/s  age %s  zones %zu/%zu/%zu", js_status_text.c_str(),
                  fps_text(snap.js_fps).c_str(), age_text(snap.js_age_s).c_str(), snap.js_left.size(),
                  snap.js_top.size(), snap.js_right.size());
    lines.push_back(box_line(js_meta, inner));
    char hue_meta[192];
    std::snprintf(hue_meta, sizeof(hue_meta), "Bottom %-8s %s fps  age %s  HueStream %s  malformed %u",
                  hue_status_text.c_str(), fps_text(snap.hue_fps).c_str(), age_text(snap.hue_age_s).c_str(),
                  snap.hue_version.empty() ? "--" : snap.hue_version.c_str(), snap.hue_bad_frames);
    lines.push_back(box_line(hue_meta, inner));
    std::string rgb_line = "RGB   ";
    for (size_t i = 0; i < bottom.size(); ++i) {
        if (i) rgb_line += "   ";
        rgb_line += bottom[i].valid ? std::to_string(bottom[i].r) + "," + std::to_string(bottom[i].g) + "," +
                                          std::to_string(bottom[i].b)
                                    : std::string("--");
    }
    lines.push_back(box_line(rgb_line, inner));
    lines.push_back(box_bottom(width));

    // ---- EVENTS -----------------------------------------------------------
    // v0.17: fills the rest of the window (at least kNoticeLines rows) and
    // scrolls back through the last kNoticeHistory lines (arrows/PgUp/PgDn).
    size_t used = lines.size() + 2 /* box top+bottom */ + 1 /* footer */;
    size_t rows = ts.rows > used ? ts.rows - used : 0;
    rows = std::max(rows, kNoticeLines);
    events_rows_ = rows;
    // v0.19: every notice becomes one or more wrapped rows; scrolling is by row.
    std::vector<std::string> wrapped;
    for (const auto& [text, count] : notices_) {
        for (auto& r : wrap_line(count > 1 ? "(" + std::to_string(count) + "x) " + text : text, inner))
            wrapped.push_back(std::move(r));
    }
    events_width_ = inner;
    events_total_rows_ = wrapped.size();
    size_t total = wrapped.size();
    size_t max_scroll = total > rows ? total - rows : 0;
    if (scroll_ > max_scroll) scroll_ = max_scroll;
    size_t end_i = total - scroll_;
    size_t begin_i = end_i > rows ? end_i - rows : 0;
    std::string events_title = "EVENTS";
    if (scroll_ > 0) {
        events_title += "  (" + std::to_string(scroll_) + " newer rows below - Down/End for the newest)";
    } else if (total > rows) {
        events_title += "  (Up/PgUp: older)";
    }
    lines.push_back(box_top(events_title, width));
    for (size_t r = 0; r < rows; ++r) {
        size_t i = begin_i + r;
        if (i < end_i) {
            lines.push_back(box_line(wrapped[i], inner));
        } else if (r == 0 && total == 0) {
            lines.push_back(box_line("Waiting for stream / TV settings", inner));
        } else {
            lines.push_back(box_line("", inner));
        }
    }
    lines.push_back(box_bottom(width));
    // v0.28: q stops the service, s starts it, x leaves; "r x2" so it does not read as a typo
    std::string footer_text =
        service_stopped_
            ? std::string("SERVICE STOPPED  s: start  r x2: restart  x: exit  t: TV ") + (show_tv_ ? "off" : "on") +
                  "  Up/Down: log"
            : std::string("q: stop  s: start  r x2: restart  x: exit  p: pair (30 s)  t: TV ") +
                  (show_tv_ ? "off" : "on") + "  Up/Down: log";
    if (service_stopped_) {
        footer_text += "   >>> no TV services, no LED output - press s to start <<<";
    } else if (snap.old_session) {
        footer_text += "   >>> TV streams from an OLD session - turn Ambilight+hue off/on on the TV <<<";
    } else if (snap.tv_dtls_lock && !snap.lamps_good) {
        footer_text += "   >>> TV LOCKED to LightGroup after a DTLS failure - RESTART THE TV <<<";
    } else if (tv_waiting && link_remaining_ms <= 0) {
        footer_text += "   >>> TV WANTS TO PAIR - press p <<<";
    } else if (link_remaining_ms > 0) {
        footer_text += "   PAIRING OPEN " + std::to_string((link_remaining_ms + 999) / 1000) +
                       " s - run Configure on the TV now";
    } else if (link_remaining_ms == 0) {
        footer_text += "   pairing: closed";
    }
    std::string footer = pad_or_truncate(footer_text, width);

    // Never more lines than the window has rows: writing past the last row
    // scrolls the alternate screen, and the next cursor-home repaint then
    // lands on top of the scrolled-up remains. Keep the top of the
    // dashboard and always the footer, same as the Python original's
    // lines[:height-2] + [footer].
    if (ts.rows >= 2 && lines.size() + 1 > ts.rows) lines.resize(ts.rows - 1);
    lines.push_back(footer);

    // Repaint in place: (full clear only when the window size changed),
    // cursor home, each line cleared to end-of-line before its content,
    // CR+LF between lines - explicit CR, so a terminal or sudo pty with
    // output newline translation off still returns to column 0 - and no
    // newline after the last line, so the screen can never scroll; then
    // clear anything left below.
    std::string out = resized ? "\x1b[2J\x1b[H" : "\x1b[H";
    for (size_t i = 0; i < lines.size(); ++i) {
        out += "\x1b[K" + lines[i];
        if (i + 1 < lines.size()) out += "\r\n";
    }
    out += "\x1b[J";
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

}  // namespace terminal_dashboard
