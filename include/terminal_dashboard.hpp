// Fixed, non-scrolling terminal dashboard - the C++ port's equivalent of
// the old Python prototype's terminal_dashboard.py. Only used when stdout
// is a real interactive terminal (main.cpp decides that); this header has
// no network/mbedTLS dependency of its own.
//
// Why this exists: without it, every [rest]/[dtls]/[ssdp]/[mdns]/[monitor]
// fprintf(stderr, ...) call anywhere in the program (there are dozens, and
// the TV's own frequent polling alone produces a steady stream of
// "[rest] ... GET /api/.../lights/1" lines) scrolls the terminal forever.
// The old Python prototype avoided this because every one of its own log
// lines was already routed through a single console_notice() helper that
// knew whether a dashboard was active. Retrofitting that here would mean
// touching every existing fprintf(stderr, ...) call site across several
// files; main.cpp instead redirects the whole process's stderr into a
// pipe once this dashboard is running (see main.cpp), so every line -
// including ones in files that know nothing about this class - becomes a
// notice() call instead of scrolling text, with no call sites changed.
#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "monitor_state.hpp"

namespace terminal_dashboard {

class TerminalDashboard {
public:
    // `monitor_url` is shown as-is under the header (e.g.
    // "http://192.0.2.30:8098/"); `version` is the AMBI_BRIDGE_VERSION
    // string shown in the header, same spot the old dashboard showed its
    // own "v1.4".
    TerminalDashboard(std::string monitor_url, std::string version);

    // Switches the real terminal to the alternate screen buffer and hides
    // the cursor, same escape codes as the Python original
    // (\x1b[?1049h\x1b[?25l). Call once, from main(), only after deciding
    // stdout is actually an interactive terminal - this class does not
    // check isatty() itself, matching how the Python original's own
    // isatty check lived in main(), not inside TerminalDashboard.
    void start();

    // Leaves the alternate screen buffer and restores the cursor
    // (\x1b[0m\x1b[?25h\x1b[?1049l). Safe to call even if start() was
    // never called, or more than once - a no-op after the first call.
    // Must be called before the process prints anything else the user
    // should actually see (e.g. a final "shutting down" message), since
    // anything printed while still in the alternate screen disappears the
    // moment this call restores the real one.
    void stop();

    // Appends one line to the small rolling notice log shown in the
    // EVENTS box (last 5 lines since v9; was 2, like the Python original). Thread-safe -
    // called from the background thread that drains the redirected stderr
    // pipe (see main.cpp), never from render() itself. Non-printable
    // bytes are replaced with spaces and the line is capped to a sane
    // length so one runaway line can't blow out the box.
    void notice(const std::string& line);

    // Repaints the whole dashboard in place (cursor-home + per-line
    // clear-to-EOL + a final clear-to-end-of-screen), never scrolling.
    // Reads the current terminal size itself (falls back to 80x24 if that
    // fails, same default the Python original used). `stream_configured`
    // mirrors the Python original's is_stream_configured gate (false only
    // makes sense if this bridge were ever built with Entertainment
    // support compiled out, which it currently never is - main.cpp always
    // passes true - but the field exists so DISABLED is distinguishable
    // from INACTIVE in the layout logic, same four-way status split as the
    // original).
    //
    // `link_remaining_ms` (v9): milliseconds left in the virtual link
    // button's pairing window (0 = closed), or -1 when pairing doesn't
    // need the button (--pairing auto). Shown in the footer next to the
    // "p: pair" key hint. `tv_waiting` (v0.15): the TV's pairing request
    // was just refused because the button wasn't pressed -> the footer
    // says "TV WANTS TO PAIR - press p".
    void render(const monitor_state::Snapshot& snap, bool stream_configured, long link_remaining_ms = -1,
                bool tv_waiting = false);

    bool active() const;

    // v0.17 keys (main.cpp's key thread): 't' hides/shows the TV picture so
    // the log gets the whole window; arrows/PgUp/PgDn scroll the log
    // (positive = older), End/'e' back to the newest line. Each asks for an
    // immediate repaint, which the render loop picks up via take_redraw().
    void toggle_tv();
    // v0.28: the footer shows the keys for a stopped service (s: start).
    void set_service_stopped(bool stopped) { service_stopped_ = stopped; }
    void scroll_log(int lines);       // +older / -newer; PAGE = one box height
    void scroll_log_pages(int pages);
    void scroll_log_newest();
    bool take_redraw() { return redraw_.exchange(false); }

private:
    std::string monitor_url_;
    std::string version_;

    mutable std::mutex mu_;
    bool active_ = false;
    std::deque<std::pair<std::string, int>> notices_;  // line, repeat count (v0.17: last 500)
    bool show_tv_ = true;
    bool service_stopped_ = false;
    size_t scroll_ = 0;        // lines up from the newest; 0 = follow the newest
    size_t events_rows_ = 5;   // rows the EVENTS box had at the last render
    // v0.19: long lines wrap onto more rows instead of being cut; scroll_
    // counts rows, not lines. Width and row total of the last render:
    size_t events_width_ = 0;
    size_t events_total_rows_ = 0;
    std::atomic<bool> redraw_{false};
    size_t last_cols_ = 0, last_rows_ = 0;  // full clear on resize
};

}  // namespace terminal_dashboard
