// Wires together everything built so far into one runnable program:
// settings + pairing persistence, the Hue REST server, the DTLS-PSK
// Entertainment stream, and SSDP/mDNS discovery. On each HueStream frame,
// runs the colour pipeline and prints a live terminal preview of the LED
// strip - there's no real LED hardware yet (see README), so this is the
// stand-in output for now.
//
// This is the LXC test build: run it on the user's Proxmox container,
// point the TV's Ambilight+hue wizard at it, and see real frames flow
// through the exact same parsing/colour/current-limiting logic that will
// later move into the ESP32/WLED usermod.
//
// Bump kVersion with every delivered build (see README "Version history")
// - it's printed as the very first line at startup specifically so a quick
// glance at the log, with zero ambiguity, says which build is actually
// running. Added after a real mix-up where a re-extracted zip silently
// kept stale pre-JointSpace source files and nothing printed said so.
#define AMBI_BRIDGE_VERSION "v0.50"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "bridge_identity.hpp"
#include "identity_store.hpp"
#include "color_pipeline.hpp"
#include "dtls_psk_server.hpp"
#include "huestream.hpp"
#include "hue_rest.hpp"
#include "jointspace_client.hpp"
#include "mdns_ssdp.hpp"
#include "monitor_server.hpp"
#include "monitor_state.hpp"
#include "pairing_store.hpp"
#include "settings.hpp"
#include "terminal_dashboard.hpp"
#include "wled_ddp.hpp"
#include "color_trace.hpp"
#include "strip_stats.hpp"
#include "login_store.hpp"
#include <vector>
#include <fstream>

namespace {

// connect()-without-sending trick to learn the outbound LAN IP, same as
// fakebridge.py's detect_ip(): no packet actually leaves the host, this
// just makes the kernel pick a local address for the route to 192.0.2.1
// (a TEST-NET-1 address, guaranteed never reachable, so it's safe to use
// purely for this address-selection side effect).
std::string detect_ip() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "127.0.0.1";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &addr.sin_addr);
    std::string ip = "127.0.0.1";
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) ip = buf;
        }
    }
    close(fd);
    return ip;
}

// Terminal preview of the REAL zones only - the actual lamp positions the
// TV's Ambilight+hue wizard sends (Bottom-Left/Middle/Right, i.e. `lamps`
// before render_strip() interpolates them out to LED_COUNT physical LEDs) -
// not the full interpolated LED strip. One wide colored block per zone,
// labeled with its name. Redraws the same terminal line in place (\r +
// clear-to-EOL) rather than scrolling a new line per change - the normal
// [rest]/[dtls]/[ssdp] log lines above it are untouched. The caller only
// calls this when the zone colours actually differ from the last redraw
// and at most twice a second, so it's a calm, roughly-live colour readout
// rather than a non-stop flicker.
// Kept deliberately narrow (~79 visible columns total for the default 3
// lamps) so the \r-based in-place redraw actually stays on one line on a
// standard 80-column terminal - a wider line wraps, and then \r only
// returns to the start of the *wrapped* row, not the true start, making it
// look like the preview is scrolling down instead of redrawing in place.
void print_zones_preview(const std::vector<color_pipeline::RGB8>& lamps,
                          const std::vector<std::string>& lamp_names,
                          int used_anchors, double estimated_ma, uint8_t seq) {
    std::string line = "\r\x1b[K";
    for (size_t i = 0; i < lamps.size(); ++i) {
        const auto& c = lamps[i];
        char block[64];
        std::snprintf(block, sizeof(block), "\x1b[48;2;%d;%d;%dm    \x1b[0m", c.r, c.g, c.b);
        line += block;
        if (i < lamp_names.size()) {
            line += " ";
            line += lamp_names[i];
        }
        line += "  ";
    }
    char stats[48];
    std::snprintf(stats, sizeof(stats), "(seq=%3d anc=%d ~%.0fmA)", seq, used_anchors, estimated_ma);
    line += stats;
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

bool lamps_differ(const std::vector<color_pipeline::RGB8>& a, const std::vector<color_pipeline::RGB8>& b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) return true;
    }
    return false;
}

std::atomic<bool> g_running{true};
volatile sig_atomic_t g_stop_requests = 0;
// v9: the terminal's original stdin mode, saved before the 'p' (pair) key
// reader switches it to unbuffered/no-echo, so every exit path - including
// the forced second-Ctrl+C one below - can put it back.
termios g_saved_termios{};
volatile sig_atomic_t g_termios_saved = 0;
// First Ctrl+C/SIGTERM: ask the main loop to shut down cleanly. A second
// one means the clean path is stuck (or the user is impatient): put the
// terminal back the way it was - leave the dashboard's alternate screen,
// cursor and line-wrap back on - and exit on the spot. Only
// async-signal-safe calls here (write, _exit). Before v7 every further
// Ctrl+C was swallowed, so a stuck shutdown left the terminal tab dead
// until it was closed.
void on_sigint(int) {
    if (g_stop_requests++ > 0) {
        static const char restore[] = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l\r\nforced exit\r\n";
        ssize_t r = write(STDOUT_FILENO, restore, sizeof(restore) - 1);
        (void)r;
        if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);  // async-signal-safe
        _exit(130);
    }
    g_running = false;
}

// ---- v0.24 (PLAN item 4): restart in place ---------------------------
// 'r' twice in the terminal, or the page's Restart button (local browser,
// no systemd unit): clean shutdown, then execv() of the newest
// build/ambi_bridge (the path this process was started from) with the
// same arguments. The new binary is tried with --version BEFORE anything
// stops, so a missing/broken build leaves the running bridge as it is.
std::atomic<bool> g_restart{false};
std::mutex g_restart_mu;
std::string g_restart_exe;  // set under g_restart_mu before g_restart

std::string self_exe_path() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    std::string p(buf, static_cast<size_t>(n));
    const std::string del = " (deleted)";  // the file was replaced by a rebuild
    if (p.size() > del.size() && p.compare(p.size() - del.size(), del.size(), del) == 0) p.resize(p.size() - del.size());
    return p;
}

// Runs `exe --version` (max ~3 s) and returns its first line, "" on failure.
std::string probe_version(const std::string& exe) {
    int fds[2];
    if (pipe(fds) != 0) return "";
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return "";
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(exe.c_str(), exe.c_str(), "--version", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char chunk[256];
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    while (std::chrono::steady_clock::now() < until) {
        ssize_t n = read(fds[0], chunk, sizeof(chunk));
        if (n > 0) {
            out.append(chunk, static_cast<size_t>(n));
            if (out.size() > 512) break;
            continue;
        }
        if (n == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return "";
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return "";
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out.resize(nl);
    return out.rfind("ambi-bridge-cpp ", 0) == 0 ? out : "";
}

std::string request_restart(const std::string& who) {
    if (g_restart.load()) return "already_restarting";
    std::string exe = self_exe_path();
    if (exe.empty() || access(exe.c_str(), X_OK) != 0) {
        std::fprintf(stderr, "[main] restart from %s refused: cannot find this program's file (%s)\n", who.c_str(),
                     exe.c_str());
        return "no_binary";
    }
    std::string ver = probe_version(exe);
    if (ver.empty()) {
        std::fprintf(stderr, "[main] restart from %s refused: %s --version failed (broken build?) - this "
                             "bridge keeps running\n", who.c_str(), exe.c_str());
        return "binary_failed";
    }
    {
        std::lock_guard<std::mutex> lk(g_restart_mu);
        g_restart_exe = exe;
    }
    std::fprintf(stderr, "[main] restart requested from %s: ambi-bridge-cpp %s -> %s (%s)\n", who.c_str(),
                 AMBI_BRIDGE_VERSION, ver.c_str(), exe.c_str());
    g_restart = true;
    g_running = false;
    return "";
}

// ---- v0.28: service Start / Stop / Exit -------------------------------
// "Stopped" = this same program started again (execv, like restart) with
// --service-stopped: only the terminal dashboard and the monitor page run;
// no REST :80, DTLS :2100, SSDP/mDNS, JointSpace polling or LED output.
// Going through execv frees every port for sure (the servers are detached
// threads that cannot be stopped one by one). Start = execv again without
// the flag. Exit = leave the program (as Ctrl+C).
bool g_service_stopped = false;           // this run was started with --service-stopped
std::atomic<bool> g_exec_stopped{false};  // the execv at exit adds --service-stopped

std::string request_service(const std::string& action, const std::string& who) {
    if (g_restart.load()) return "already_restarting";
    if (action == "exit") {
        std::fprintf(stderr, "[main] exit requested from %s\n", who.c_str());
        g_running = false;
        return "";
    }
    if (action == "restart") {
        g_exec_stopped = false;
        return request_restart(who);
    }
    if (action == "stop") {
        if (g_service_stopped) return "already_stopped";
        // the running image itself (no surprise build change on a stop)
        {
            std::lock_guard<std::mutex> lk(g_restart_mu);
            g_restart_exe = "/proc/self/exe";
        }
        std::fprintf(stderr, "[main] stop requested from %s: TV services and LED output stop; the terminal and "
                             "the monitor page stay (start again: s, or Start on the page)\n", who.c_str());
        g_exec_stopped = true;
        g_restart = true;
        g_running = false;
        return "";
    }
    if (action == "start") {
        if (!g_service_stopped) return "already_running";
        // the newest build when it works, else this same one
        std::string exe = self_exe_path();
        std::string ver = exe.empty() || access(exe.c_str(), X_OK) != 0 ? "" : probe_version(exe);
        if (ver.empty()) {
            std::fprintf(stderr, "[main] start: the build on disk (%s) does not run - starting this same build\n",
                         exe.c_str());
            exe = "/proc/self/exe";
            ver = std::string("ambi-bridge-cpp ") + AMBI_BRIDGE_VERSION;
        }
        {
            std::lock_guard<std::mutex> lk(g_restart_mu);
            g_restart_exe = exe;
        }
        std::fprintf(stderr, "[main] start requested from %s: %s\n", who.c_str(), ver.c_str());
        g_exec_stopped = false;
        g_restart = true;
        g_running = false;
        return "";
    }
    return "unknown_action";
}

}  // namespace

int main(int argc, char** argv) {
    std::string state_dir = "/var/lib/ambi-bridge-cpp";
    uint16_t rest_port = 80;
    uint16_t stream_port = 2100;
    std::string tv_ip;  // empty = accept pairing from any source (see README)
    int lamp_count = 0;  // 0 = from the active identity (v0.17); --lamps overrides it for this run
    uint16_t monitor_port = 8098;
    // Empty by default: Stop/Restart on the monitor page are only safe to
    // offer once this process is actually running *as* that systemd unit -
    // pass the real unit name (e.g. "ambi-bridge.service") from the
    // ExecStart line in systemd/ambi-bridge.service, not when running this
    // by hand from a terminal.
    std::string systemd_unit;
    // JointSpace (the TV's own left/top/right screen-capture zones, read
    // via its local HTTPS API on port 1926 - see jointspace_client.hpp)
    // reuses --tv-ip as the target address. Credentials can be seeded here
    // for a first run, but the monitor page's own credentials form
    // (loopback-only, see monitor_server.cpp) is the normal way to set
    // them - once saved there, they persist in --state-dir and these two
    // flags stop mattering.
    std::string tv_user, tv_pass;
    double js_interval_s = 0.3;  // same default as the old bridge's --js-interval
    // "auto" (default): a fixed, non-scrolling dashboard on a real
    // interactive terminal (same idea as the old Python prototype's
    // terminal_dashboard.py, plus actual colour swatches for the bottom
    // lamps, not just RGB numbers), falling back to the plain scrolling
    // single-line colour preview anywhere else (systemd journal, a piped
    // log file, `script`, etc.) - matching the old prototype's own
    // --console auto/dashboard/plain/quiet choices.
    std::string console_mode = "auto";
    // v9: "button" (default) = pairing only while the virtual link button's
    // 30 s window is open (monitor page "Pair" button or 'p' in the
    // terminal), like a real Hue bridge; "auto" = accept every pairing
    // request from the TV's IP, the pre-v9 behaviour.
    std::string pairing_mode = "button";
    // v11 (Phase 1 of docs/PLAN-NEXT.md): optional DDP output of every
    // frame's render_strip() result to a WLED controller. Empty = off,
    // nothing is sent. 4048 is WLED's fixed DDP port.
    std::string wled_ip;
    uint16_t wled_port = wled_ddp::kDefaultPort;
    // v0.35: debug colour trace (see include/color_trace.hpp). Off unless given.
    std::string color_trace_path;
    double color_trace_seconds = 600.0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (arg == flag && i + 1 < argc) return argv[++i];
            return nullptr;
        };
        if (const char* v = next("--state-dir")) state_dir = v;
        else if (const char* v = next("--port")) rest_port = static_cast<uint16_t>(std::atoi(v));
        else if (const char* v = next("--stream-port")) stream_port = static_cast<uint16_t>(std::atoi(v));
        else if (const char* v = next("--tv-ip")) tv_ip = v;
        else if (const char* v = next("--lamps")) lamp_count = std::atoi(v);
        else if (const char* v = next("--monitor-port")) monitor_port = static_cast<uint16_t>(std::atoi(v));
        else if (const char* v = next("--systemd-unit")) systemd_unit = v;
        else if (const char* v = next("--tv-user")) tv_user = v;
        else if (const char* v = next("--tv-pass")) tv_pass = v;
        else if (const char* v = next("--js-interval")) js_interval_s = std::atof(v);
        else if (const char* v = next("--console")) console_mode = v;
        else if (const char* v = next("--pairing")) pairing_mode = v;
        else if (const char* v = next("--wled-ip")) wled_ip = v;
        else if (const char* v = next("--wled-port")) wled_port = static_cast<uint16_t>(std::atoi(v));
        else if (const char* v = next("--color-trace")) color_trace_path = v;
        else if (const char* v = next("--color-trace-seconds")) color_trace_seconds = std::atof(v);
        // v0.32 (Mode B): --ddp-target IP[:PORT] = the same DDP output to a
        // stock WLED, in one option. Off unless given.
        else if (const char* v = next("--ddp-target")) {
            std::string t = v;
            const size_t colon = t.rfind(':');
            if (colon != std::string::npos) {
                const int port_num = std::atoi(t.c_str() + colon + 1);
                if (port_num < 1 || port_num > 65535) {
                    std::fprintf(stderr, "--ddp-target %s: the port must be 1..65535\n", v);
                    return 2;
                }
                wled_port = static_cast<uint16_t>(port_num);
                t.resize(colon);
            }
            wled_ip = t;
        }
        else if (arg == "--service-stopped") g_service_stopped = true;  // v0.28: set by Stop (execv)
        else if (arg == "--help" || arg == "-h") {
            std::printf("usage: %s [--state-dir DIR] [--port N] [--stream-port N] [--tv-ip IP] [--lamps 3|5] "
                        "[--monitor-port N] [--systemd-unit NAME] [--tv-user NAME] [--tv-pass PASS] "
                        "[--js-interval SECONDS] [--console auto|dashboard|plain|quiet] [--pairing button|auto] "
                        "[--ddp-target IP[:PORT] (DDP to a stock WLED, default port 4048; off when not given)] "
                        "[--wled-ip IP] [--wled-port N] (the same, older spelling) "
                        "[--color-trace FILE [--color-trace-seconds N]] (debug: one JSON line per frame, "
                        "stops after N s, default 600) [--service-stopped] [--version]\n",
                         argv[0]);
            return 0;
        } else if (arg == "--version") {
            std::printf("ambi-bridge-cpp %s\n", AMBI_BRIDGE_VERSION);
            return 0;
        }
    }

    if (pairing_mode != "button" && pairing_mode != "auto") {
        std::fprintf(stderr, "--pairing must be \"button\" or \"auto\" (got \"%s\")\n", pairing_mode.c_str());
        return 2;
    }
    const bool pairing_needs_button = pairing_mode == "button";

    // Opened before any thread starts, so a bad --wled-ip exits cleanly.
    wled_ddp::Sender wled_sender;
    color_trace::Trace color_trace_obj;
    if (!color_trace_path.empty() && !color_trace_obj.open(color_trace_path, color_trace_seconds)) return 2;
    if (!wled_ip.empty() && !wled_sender.open(wled_ip, wled_port)) return 2;
    if (wled_ip.empty() && wled_port != wled_ddp::kDefaultPort)
        std::fprintf(stderr, "[wled] --wled-port given without --wled-ip: no DDP output\n");

    // Decided early (console_mode only needs the CLI flag) so the on_frame
    // handler below can gate the old scrolling single-line colour preview
    // on it; the dashboard object itself (and the stderr redirect it
    // needs) is only actually set up much later, right before the main
    // loop, after the one-time startup banner has had a chance to scroll
    // by normally - see the comment there.
    bool stdout_is_tty = ::isatty(STDOUT_FILENO) != 0;
    const char* term_env = std::getenv("TERM");
    bool term_dumb = term_env && std::string(term_env) == "dumb";
    bool want_dashboard = console_mode == "dashboard" || (console_mode == "auto" && stdout_is_tty && !term_dumb);
    bool want_preview = console_mode != "quiet" && !want_dashboard;

    if (mkdir(state_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "cannot create state dir %s: %s\n", state_dir.c_str(), std::strerror(errno));
        return 1;
    }
    // One bridge per state dir. Until v8 a second copy started while another
    // was still running (e.g. one left behind in a closed terminal) failed
    // to bind port 80 but kept running everything else - including a second
    // JointSpace poller hitting the TV. flock() is released by the kernel
    // when this process dies in any way, kill -9 included; O_CLOEXEC keeps
    // the avahi-publish children from inheriting (and so holding) the lock.
    {
        std::string lock_path = state_dir + "/ambi_bridge.lock";
        int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (lock_fd >= 0 && ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            char other[32] = {0};
            ssize_t r = ::read(lock_fd, other, sizeof(other) - 1);
            (void)r;
            for (char& ch : other)
                if (ch == '\n') ch = '\0';
            std::fprintf(stderr,
                         "another ambi_bridge is already running with state dir %s (pid %s).\n"
                         "Stop it first:  sudo pkill -f ambi_bridge\n",
                         state_dir.c_str(), other[0] ? other : "?");
            return 1;
        }
        if (lock_fd >= 0) {
            if (::ftruncate(lock_fd, 0) == 0) {
                std::string pid = std::to_string(::getpid()) + "\n";
                ssize_t w = ::pwrite(lock_fd, pid.data(), pid.size(), 0);
                (void)w;
            }
            // lock_fd intentionally never closed: the lock lives as long as we do.
        }
    }
    std::string settings_path = state_dir + "/settings.json";
    // v0.17: identity (MAC, name, lamp count) chosen on the monitor page,
    // stored in identity.json; each identity has its own pairing/groups.
    identity_store::Store id_store = identity_store::load(state_dir);
    identity_store::Entry active_id = *id_store.find(id_store.active);
    std::string identity_dir = identity_store::dir_for(state_dir, active_id.mac);
    if (identity_dir != state_dir) {
        std::string parent = state_dir + "/identities";
        mkdir(parent.c_str(), 0755);
        if (mkdir(identity_dir.c_str(), 0755) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "cannot create identity dir %s: %s\n", identity_dir.c_str(), std::strerror(errno));
            return 1;
        }
    }
    if (lamp_count == 0) {
        lamp_count = active_id.lamps;
    } else if (lamp_count != active_id.lamps) {
        std::fprintf(stderr, "[main] --lamps %d overrides the identity's %d lamps for this run\n", lamp_count,
                     active_id.lamps);
    }
    if (lamp_count != 3 && lamp_count != 5) {
        std::fprintf(stderr, "--lamps must be 3 or 5\n");
        return 2;
    }
    std::string pairing_path = identity_dir + "/pairing.json";

    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);
    // Closing the terminal window (SIGHUP, relayed by sudo) now shuts down
    // cleanly too. Until v8 a bridge could outlive the terminal it was
    // started from and keep polling the TV with nobody watching it.
    std::signal(SIGHUP, on_sigint);
    // A browser tab closed or refreshed mid-response (or the TV dropping a
    // REST connection) makes the next send() on that socket raise SIGPIPE,
    // whose default action silently kills the whole process - no
    // "Aborted", no message, the bridge just disappears. Ignoring it turns
    // that into a plain EPIPE error from send(), which every send loop in
    // this program already handles by giving up on that one connection.
    // (The old Python bridge never had this problem: CPython ignores
    // SIGPIPE at startup by default.)
    std::signal(SIGPIPE, SIG_IGN);

    std::string ip = detect_ip();
    auto identity = bridge_identity::make_identity(ip, rest_port, active_id.mac, active_id.name);
    identity.apiversion = "1.67.0";    // >= 1.22 -> TV treats this bridge as streaming-capable
    identity.swversion = "1967054020";
    identity_store::Shared id_shared(state_dir, {active_id.mac, active_id.name, lamp_count});
    std::fprintf(stderr, "[main] identity \"%s\" %s, %d lamps, pairing in %s\n", active_id.name.c_str(),
                 active_id.mac.c_str(), lamp_count, identity_dir.c_str());

    std::fprintf(stderr, "ambi-bridge-cpp %s starting: ip=%s mac=%s bridgeid=%s rest_port=%u stream_port=%u\n",
                 AMBI_BRIDGE_VERSION, ip.c_str(), identity.mac.c_str(), identity.bridgeid.c_str(), rest_port,
                 stream_port);
    std::fprintf(stderr, "state dir: %s\n", state_dir.c_str());

    settings::SharedSettings strip_settings(settings_path);
    // v0.23 (NEXT 6): per-LED totals + the limiter's compromise episodes,
    // <state-dir>/strip_stats.json (saved every minute and at exit).
    strip_stats::Stats strip_stats_obj(state_dir + "/strip_stats.json");
    pairing_store::ThreadSafeStore pairing(pairing_path);
    std::fprintf(stderr, "pairing store: %zu known user(s)\n", pairing.user_count());
    std::fprintf(stderr, "pairing: %s\n",
                 pairing_needs_button
                     ? "link button required - press Pair on the monitor page or 'p' here, then Configure on the TV"
                     : "auto (every request from the TV is accepted)");

    // ---- discovery: SSDP (thread) + mDNS (fire-and-forget subprocess) -----
    // v0.19: who asks (SSDP + description.xml) + the "answer only Hue searches" switch.
    discovery::Registry discovery_reg(state_dir);
    const bool svc = !g_service_stopped;  // v0.28: false = only terminal + monitor page
    // v0.32: AMBI_NO_DISCOVERY=1 (tests only) = no SSDP answers and no mDNS, so
    // a sandbox bridge on test ports never shows the TV / the LAN a second
    // "bridge" with the real one's MAC while the real one is running.
    const char* no_disc_env = std::getenv("AMBI_NO_DISCOVERY");
    const bool no_discovery = no_disc_env && std::string(no_disc_env) == "1";
    if (no_discovery) std::fprintf(stderr, "[main] AMBI_NO_DISCOVERY=1: SSDP and mDNS are off (test run)\n");
    if (svc && !no_discovery) {
        std::thread ssdp_thread([&identity, &discovery_reg] { mdns_ssdp::run_ssdp_responder(identity, &discovery_reg); });
        ssdp_thread.detach();
        mdns_ssdp::start_mdns(identity);
    }

    // ---- v0.29: stream.active across a restart -------------------------
    // The TV's DTLS session is saved at stop (dtls_session.bin) and continued
    // by the next run; the TV does not send stream.active=true again, so the
    // group + owner that were active are kept in stream_resume.txt and
    // restored when the first frame of the continued session is decrypted.
    const std::string dtls_session_path = identity_dir + "/dtls_session.bin";
    const std::string stream_resume_path = identity_dir + "/stream_resume.txt";
    std::mutex stream_mu;
    // v0.31: REST asks the DTLS server to end the open session (see dtls_psk::Options::drop_session)
    std::atomic<bool> dtls_drop{false};
    std::string stream_group, stream_owner;  // the active one (empty = none)
    std::string resume_group, resume_owner;  // read from the previous run
    std::atomic<bool> resume_ready{false};
    {
        std::ifstream rf(stream_resume_path);
        if (rf) std::getline(rf, resume_group), std::getline(rf, resume_owner);
        std::remove(stream_resume_path.c_str());
    }

    // ---- REST server --------------------------------------------------
    hue_rest::Options rest_opts;
    rest_opts.port = rest_port;
    rest_opts.tv_ip = tv_ip;
    rest_opts.require_link_button = pairing_needs_button;
    rest_opts.groups_store_path = identity_dir + "/groups.json";
    if (lamp_count == 5) {
        rest_opts.lamp_names = {"Bottom-Left", "Bottom-CenterLeft", "Bottom-Middle",
                                 "Bottom-CenterRight", "Bottom-Right"};
    }
    rest_opts.apiversion = identity.apiversion;
    rest_opts.swversion = identity.swversion;
    rest_opts.take_resumed_stream = [&](std::string& g, std::string& o) {
        if (!resume_ready.exchange(false)) return false;
        std::lock_guard<std::mutex> lk(stream_mu);
        g = resume_group;
        o = resume_owner;
        stream_group = g;
        stream_owner = o;
        return !g.empty();
    };
    const std::vector<std::string> lamp_names = rest_opts.lamp_names;

    monitor_state::SharedState monitor_state_obj;
    monitor_state_obj.set_lamp_names(lamp_names);  // shows names before the first real frame arrives
    if (wled_sender.is_open()) monitor_state_obj.set_wled_target(wled_ip + ":" + std::to_string(wled_port));

    // ---- JointSpace (TV's own left/top/right screen-capture zones) ----
    // Read-only, reuses --tv-ip; credentials come from --tv-user/--tv-pass
    // as a bootstrap, or from the monitor page's own (loopback-only)
    // settings form, which is what persists them to --state-dir.
    jointspace::Settings js_settings(tv_ip, tv_user, tv_pass, state_dir + "/jointspace_credentials.json");
    std::thread js_thread;
    if (svc) js_thread = std::thread([&] {
        int interval_ms = std::max(50, static_cast<int>(js_interval_s * 1000));
        jointspace::run_poll_loop(
            js_settings, interval_ms,
            [&monitor_state_obj](bool ok, const std::string& status, const std::string& error,
                                  std::vector<monitor_state::Zone> left, std::vector<monitor_state::Zone> top,
                                  std::vector<monitor_state::Zone> right) {
                monitor_state_obj.set_jointspace_result(ok, status, error, std::move(left), std::move(top),
                                                         std::move(right));
            });
    });
    if (svc) js_thread.detach();

    rest_opts.on_lightgroup_used = [&monitor_state_obj] { monitor_state_obj.note_lightgroup_used(); };
    // v0.37: TV streams from an old DTLS session (we were restarted / killed while it streamed):
    // fail its lamp check so it reconnects by itself instead of Ambilight+hue off/on.
    rest_opts.fail_lamp_check = [&monitor_state_obj](const std::string& src) {
        const bool nudge = monitor_state_obj.old_session_nudge(src);
        if (nudge) std::fprintf(stderr, "[rest] %s streams from an old session: lamp check -> 503 (TV reconnects)\n", src.c_str());
        return nudge;
    };
    std::thread rest_thread;
    if (svc) rest_thread = std::thread([&] {
        hue_rest::run_server(
            rest_opts, identity, pairing, pairing_path,
            [&monitor_state_obj, &stream_mu, &stream_group, &stream_owner, &dtls_drop](
                const std::string& user, const std::string& group, bool active,
                const std::vector<std::string>& lamp_ids) {
                {
                    std::lock_guard<std::mutex> lk(stream_mu);
                    stream_group = active ? group : "";
                    stream_owner = active ? user : "";
                }
                std::fprintf(stderr, "\n[main] group %s stream.active=%s (user=%.8s..., %zu lamp(s))\n",
                             group.c_str(), active ? "true" : "false", user.c_str(), lamp_ids.size());
                monitor_state_obj.set_stream_active(active);
                // v0.31: the TV sends false, then true, then starts a NEW handshake
                // and gives it 3 s. On false: free the DTLS server right away. On
                // true: start that 3 s clock.
                if (active) {
                    monitor_state_obj.note_stream_requested();
                } else {
                    monitor_state_obj.note_stream_released();
                    dtls_drop = true;
                }
            },
            &strip_settings, &discovery_reg);
    });
    if (svc) rest_thread.detach();

    // ---- monitor dashboard (this bridge's own extra feature, see
    // monitor_server.hpp) ------------------------------------------------
    monitor_server::Options monitor_opts;
    monitor_opts.port = monitor_port;
    monitor_opts.systemd_unit = systemd_unit;
    monitor_opts.pairing_requires_button = pairing_needs_button;
    monitor_opts.version = AMBI_BRIDGE_VERSION;
    monitor_opts.request_restart = request_restart;
    monitor_opts.request_service = request_service;
    monitor_opts.service_state = [] { return std::string(g_service_stopped ? "stopped" : "running"); };
    // v0.28: login for edits from other devices (<state-dir>/monitor_login.json)
    static login_store::Store login(state_dir);
    monitor_opts.login = &login;
    std::fprintf(stderr, "monitor login: %s\n",
                 login.configured() ? ("set (user " + login.username() + "): other devices can edit after logging in").c_str()
                                    : "not set - edits only from this Linux machine's browser (set one on the Bridge tab)");
    std::thread monitor_thread(
        [&] { monitor_server::run_server(monitor_opts, identity, pairing, monitor_state_obj, js_settings,
                                   &strip_settings, &id_shared, &discovery_reg, &strip_stats_obj); });
    monitor_thread.detach();

    // ---- DTLS-PSK Entertainment stream -> colour pipeline -> preview ----
    std::map<std::string, color_pipeline::LampDedupState> dedup_xy;
    std::map<std::string, color_pipeline::LampDedupState> dedup_rgb;
    std::mutex frame_mu;
    auto last_print = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    std::vector<color_pipeline::RGB8> last_shown_lamps;
    int last_used_anchors = -1;  // -1 = no strip rendered yet this run
    double last_ma = 0.0;
    // Last rendered strip, re-sent to WLED on EVERY frame (not only when a
    // colour changed): WLED leaves realtime mode after 2.5 s without
    // packets (wled.h:436), so a still picture must keep the stream going.
    std::vector<color_pipeline::RGB8> last_leds;
    // v0.13 (Phase 2): last_leds is the TARGET (after the current limit);
    // what is shown/sent is out_leds, which follows it with the rise limit
    // (rise_ms) and drops immediately. Updated on every frame.
    std::vector<color_pipeline::RGB8> out_leds;
    color_pipeline::RenderInfo last_info;
    std::vector<color_pipeline::RGB8> last_compare;  // same frame with peak_emphasis = 0, page only
    double last_compare_ma = 0.0;
    auto last_out_time = std::chrono::steady_clock::now();
    color_pipeline::RiseState rise_state;  // v0.33: soft start after a start / pause (rise_from_shown)
    color_pipeline::DarkFadeState dark_fade;  // v0.42: soft fade when the picture goes dark
    // v0.33: set (under frame_mu) once the goodbye black frame went out at
    // stop / exit: nothing is sent to the strip after it.
    bool output_closed = false;
    // v0.33: per-LED wear (0..1) for the wear-aware limiter, from the all-time
    // mAh of strip_stats; computed again once a minute, and only while
    // wear_strength > 0.
    std::vector<double> wear_cache;
    auto wear_cache_time = std::chrono::steady_clock::now() - std::chrono::hours(1);
    // v0.23 (NEXT 5): the clock of the rolling peaks (seconds since start).
    const auto peaks_clock_start = std::chrono::steady_clock::now();
    // v0.24 (review item 1): stream loss. Until v0.23 the strip was only
    // sent on Hue frames, so when the TV stopped, WLED held the last frame
    // for 2.5 s and then jumped to its own preset. Now an output ticker
    // (below) holds, fades to black and keeps black going for a while.
    bool have_hue_frame = false;
    auto last_hue_frame = std::chrono::steady_clock::now();
    auto last_wled_send = last_hue_frame;
    std::string out_phase = "live";
    monitor_state::StripInfo last_si;
    // v0.24: the JointSpace corner share fades (1 s) instead of switching.
    double corner_weight = 0.0;
    color_pipeline::Corners corner_last;
    auto corner_time = last_hue_frame;
    const double kCornerFadeMs = 1000.0;
    // Calm, not-constantly-redrawing preview: even on a 25-60fps stream,
    // only redraw the terminal line at most twice a second, and only when
    // the zone colours actually changed since the last redraw.
    const auto kPreviewInterval = std::chrono::milliseconds(500);

    auto on_frame = [&](const uint8_t* data, size_t len) {
        huestream::Frame frame;
        if (!huestream::parse_frame(data, len, frame)) {
            monitor_state_obj.note_bad_frame();
            return;
        }

        std::lock_guard<std::mutex> lk(frame_mu);
        {
            auto now = std::chrono::steady_clock::now();
            if (have_hue_frame && out_phase != "live") {
                std::fprintf(stderr, "[out] Hue stream back after %.1f s (strip was: %s)\n",
                             std::chrono::duration<double>(now - last_hue_frame).count(), out_phase.c_str());
                // rise limit from what the strip shows now (held/faded/black), one frame step
                if (out_phase != "hold") last_out_time = now - std::chrono::milliseconds(40);
            }
            out_phase = "live";
            have_hue_frame = true;
            last_hue_frame = now;
        }
        // Lamps in ascending lamp_id order, matching the locations the REST
        // server handed out (bottom_locations() assigns left-to-right by
        // the order lights were listed, lamp "1".."N"). Until v6 this
        // comment was true only if the TV happened to send the channels in
        // that order - nothing actually sorted them, so a frame listing
        // lamp 3 before lamp 1 would have swapped left and right on the
        // strip. The old Python bridge keyed everything by lamp id, so it
        // was immune; sort here to match.
        std::sort(frame.channels.begin(), frame.channels.end(),
                  [](const huestream::Channel& a, const huestream::Channel& b) {
                      return std::atoi(a.lamp_id.c_str()) < std::atoi(b.lamp_id.c_str());
                  });
        std::vector<color_pipeline::RGB8> lamps;
        std::vector<monitor_state::LampSnapshot> lamp_snapshots;
        bool any_changed = false;
        for (auto& ch : frame.channels) {
            color_pipeline::RGB8 rgb;
            monitor_state::LampSnapshot snap;
            snap.name = lamp_snapshots.size() < lamp_names.size() ? lamp_names[lamp_snapshots.size()]
                                                                    : ("Lamp " + std::to_string(lamp_snapshots.size() + 1));
            snap.raw16[0] = ch.a;
            snap.raw16[1] = ch.b;
            snap.raw16[2] = ch.c;
            if (frame.colorspace == 1) {
                double x = ch.a / 65535.0, y = ch.b / 65535.0, bri = ch.c / 65535.0 * 254.0;
                if (color_pipeline::xy_bri_changed(dedup_xy[ch.lamp_id], x, y, bri)) any_changed = true;
                rgb = color_pipeline::xy_bri_to_rgb(x, y, bri);
                snap.has_xy = true;
                snap.x = x;
                snap.y = y;
                snap.has_bri8 = true;
                snap.bri8 = ch.c / 257.0;  // 16-bit -> ~8-bit scale, same as the old bridge
            } else {
                rgb = {color_pipeline::rgb16_to_8(ch.a), color_pipeline::rgb16_to_8(ch.b),
                       color_pipeline::rgb16_to_8(ch.c)};
                if (color_pipeline::rgb_changed(dedup_rgb[ch.lamp_id], rgb.r, rgb.g, rgb.b)) any_changed = true;
            }
            snap.rgb = rgb;
            lamps.push_back(rgb);
            lamp_snapshots.push_back(snap);
        }
        if (lamps.empty()) return;

        // The dedup tolerances (0.03 CIE / 16 brightness) only decide
        // whether to redo the LED-strip render + current estimate below.
        // They must NOT gate the monitor state update: until v5 this
        // function returned early here on every "unchanged" frame, so the
        // monitor's freshness timestamp, frames/s counter and lamp colours
        // only moved when a colour changed by more than the tolerance. On a
        // calm scene the TV keeps streaming ~25 frames/s, but the monitor
        // saw maybe one "frame" every second or two - hence the spiky
        // 0-1200 ms age chart, "-" frames/s, total "frames: 3", and lamps
        // flipping between live and "stale"/inactive whenever the picture
        // held still. The old Python bridge recorded every parsed frame
        // (STREAM_STAT["last"]/["times"] in stream_frame()), which is why
        // the same TV looked smooth there. Now: every parsed frame counts,
        // with its exact colour.
        color_pipeline::Settings s = strip_settings.get();
        // v0.18: render on EVERY frame (it is cheap: 43 LEDs). Until now only
        // when a lamp colour changed, so a settings change from the page, or a
        // change of the TV's side colours (corner_mode "side"), reached the
        // strip only at the next colour change of the bottom lamps.
        {
            // Corner colours: the JointSpace zone touching the bottom on each
            // side (left[0], right[last] in JointSpace order, like the old
            // monitor), 0..23 assumed linear with white = 23 -> linear LED.
            color_pipeline::Corners corners;
            {
                // v0.24: live JointSpace -> its corner colours, share fading
                // up to 1; stale/off -> the last colours, share fading to 0.
                bool js_live = false;
                const bool want_sides = color_pipeline::wants_side_colours(s, static_cast<int>(lamps.size()));
                if (want_sides) {
                    monitor_state::Snapshot js = monitor_state_obj.get();
                    // v0.33: corner_js_curve / corner_js_full_scale (defaults = the
                    // v0.18..v0.32 linear x 255 / 23, bit-identical)
                    auto to_led = [&s](const monitor_state::Zone& z) {
                        return color_pipeline::jointspace_to_led(z.r, z.g, z.b, s);
                    };
                    if (!js.js_left.empty() || !js.js_right.empty()) {
                        js_live = true;
                        corner_last.has_left = !js.js_left.empty();
                        corner_last.has_right = !js.js_right.empty();
                        if (corner_last.has_left) corner_last.left = to_led(js.js_left.front());
                        if (corner_last.has_right) corner_last.right = to_led(js.js_right.back());
                    }
                }
                auto now = std::chrono::steady_clock::now();
                double step = std::chrono::duration<double, std::milli>(now - corner_time).count() / kCornerFadeMs;
                corner_time = now;
                if (!want_sides) corner_weight = 0.0;  // mode off: no fade
                else if (js_live) corner_weight = std::min(1.0, corner_weight + step);
                else corner_weight = std::max(0.0, corner_weight - step);
                if (corner_weight > 0.0) {
                    corners = corner_last;
                    corners.weight = corner_weight;
                }
            }
            // v0.12: XY frames arrive as display sRGB (xy_bri_to_rgb);
            // the strip needs linear PWM values. RGB-colorspace frames are
            // already direct levels (same rule as the page's
            // Color.hueLinear), so they go through unchanged.
            std::vector<color_pipeline::RGB8> led_lamps = lamps;
            if (frame.colorspace == 1)
                for (auto& c : led_lamps) c = color_pipeline::srgb_to_led(c);
            const double peaks_t =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - peaks_clock_start).count();
            const std::vector<double>* wear = nullptr;
            if (s.wear_strength > 0.0) {
                const auto wnow = std::chrono::steady_clock::now();
                if (wnow - wear_cache_time >= std::chrono::seconds(60) || static_cast<int>(wear_cache.size()) != s.led_count) {
                    wear_cache = color_pipeline::wear_from_mah(strip_stats_obj.install_mah(), s.led_count);  // v0.41: since the strip was mounted
                    wear_cache_time = wnow;
                }
                wear = &wear_cache;
            }
            last_leds = color_pipeline::render_strip(led_lamps, s, &last_info, &corners, peaks_t, wear);
            last_used_anchors = last_info.used_anchors;
            color_pipeline::Settings s0 = s;
            s0.peak_emphasis = 0.0;
            last_compare = color_pipeline::render_strip(led_lamps, s0, nullptr, &corners);
            last_compare_ma = color_pipeline::estimate_current_ma(last_compare, s);
        }
        {
            auto now = std::chrono::steady_clock::now();
            double dt_ms = std::chrono::duration<double, std::milli>(now - last_out_time).count();
            last_out_time = now;
            // v0.33: rise_from_shown = limit_rise for flowing frames, and a ramp (not
            // one step to the full budget) for the first frame and after a pause.
            {
                const std::vector<color_pipeline::RGB8> shown = out_leds;
                out_leds = color_pipeline::rise_from_shown(rise_state, shown, last_leds, dt_ms, s.rise_ms, s.fade_in_ms);
                if (rise_state.remaining_ms <= 0.0) out_leds = color_pipeline::smooth_small_changes(shown, out_leds, dt_ms, s);  // v0.44
                out_leds = color_pipeline::fade_to_dark(dark_fade, shown, out_leds, dt_ms, s.fade_out_ms);  // v0.42
            }
            last_ma = color_pipeline::estimate_current_ma(out_leds, s);
            strip_stats_obj.add_frame(
                out_leds, s, last_info,
                std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(),
                dt_ms / 1000.0);
            monitor_state::StripInfo si;
            si.budget_ma = s.current_budget_ma;
            si.ma_per_led_full_white = s.ma_per_led_full_white;
            si.peak_emphasis = s.peak_emphasis;
            si.rise_ms = s.rise_ms;
            si.lamp_inset = s.lamp_inset;
            si.blend_mode = color_pipeline::blend_mode_name(s.blend_mode);
            si.corner_mode = color_pipeline::corner_mode_name(s.corner_mode);
            si.blend_softness = s.blend_softness;
            si.peak_drift_leds = color_pipeline::peak_drift_amplitude(s, static_cast<int>(lamps.size()));
            si.peak_drift_auto = s.peak_drift_leds < 0.0;
            si.peak_drift_shape = color_pipeline::drift_shape_name(s.peak_drift_shape);
            si.end_lamps = color_pipeline::end_lamps_active(s, static_cast<int>(lamps.size()));
            si.corner_weight = corner_weight;
            si.strip_layout = color_pipeline::strip_layout_name(s.strip_layout);
            si.edge_leds = s.edge_leds;
            si.edge_strength = s.edge_strength;
            si.edge_source = color_pipeline::edge_source_name(
                color_pipeline::edge_source_resolved(s, static_cast<int>(lamps.size())));
            si.edge_source_setting = color_pipeline::edge_source_name(s.edge_source);
            si.output_phase = "live";
            si.peak_offset_leds = last_info.peak_offset_leds;
            si.wear_strength = s.wear_strength;
            si.wear_applied = last_info.wear_applied;
            si.wanted_ma = last_info.wanted_ma;
            si.limit_mode = last_info.limit_mode;
            si.peak_scale = last_info.peak_scale;
            si.min_scale = last_info.min_scale;
            si.peak_leds = last_info.peak_leds;
            si.target_ma = color_pipeline::estimate_current_ma(last_leds, s);
            si.compare_pe0 = last_compare;
            si.compare_pe0_ma = last_compare_ma;
            monitor_state_obj.update_strip(out_leds, si);
            last_si = si;
        }
        bool ddp_sent = false;
        if (wled_sender.is_open() && !output_closed) {
            bool sent = wled_sender.send(out_leds);
            ddp_sent = sent;
            monitor_state_obj.note_wled_send(sent, wled_sender.last_error());
            last_wled_send = std::chrono::steady_clock::now();
        }
        if (color_trace_obj.active()) {
            color_trace::Frame tf;
            tf.seq = frame.seq;
            tf.colorspace = frame.colorspace;
            for (size_t i = 0; i < frame.channels.size() && i < lamps.size(); ++i) {
                color_trace::Lamp tl;
                tl.id = frame.channels[i].lamp_id;
                tl.raw[0] = frame.channels[i].a;
                tl.raw[1] = frame.channels[i].b;
                tl.raw[2] = frame.channels[i].c;
                tl.rgb = lamps[i];
                tl.led = frame.colorspace == 1 ? color_pipeline::srgb_to_led(lamps[i]) : lamps[i];
                tf.lamps.push_back(tl);
            }
            monitor_state::Snapshot js = monitor_state_obj.get();
            tf.js_age_s = js.js_age_s;
            auto copy_zones = [](const std::vector<monitor_state::Zone>& in, std::vector<color_trace::Rgb3>& out) {
                for (const auto& z : in) out.push_back({z.r, z.g, z.b});
            };
            copy_zones(js.js_left, tf.js_left);
            copy_zones(js.js_top, tf.js_top);
            copy_zones(js.js_right, tf.js_right);
            for (const auto& pkt : wled_ddp::build_packets(out_leds, 1))
                tf.ddp_payload.insert(tf.ddp_payload.end(), pkt.begin() + wled_ddp::kHeaderLen, pkt.end());
            tf.ddp_sent = ddp_sent;
            tf.estimated_ma = last_ma;
            color_trace_obj.write(tf);
        }
        int used_anchors = last_used_anchors;
        double ma = last_ma;
        monitor_state_obj.update_frame(lamp_snapshots, used_anchors, ma, frame.seq, frame.major, frame.minor,
                                        frame.colorspace);
        if (!any_changed) return;

        // Redraw the same line in place only on an actual colour change,
        // capped at 2x/second - a calm, roughly-live readout, not a
        // non-stop flicker and not a scrolling line-per-change log.
        auto now = std::chrono::steady_clock::now();
        if (want_preview && lamps_differ(lamps, last_shown_lamps) && now - last_print > kPreviewInterval) {
            print_zones_preview(lamps, lamp_names, used_anchors, ma, frame.seq);
            last_print = now;
            last_shown_lamps = lamps;
        }
    };

    // ---- v0.24 (review item 1): output ticker for stream loss ----------
    // Every 40 ms: if no Hue frame came for a while, hold the last strip
    // (resent every 0.5 s), fade it to black, send black every 0.5 s for
    // loss_black_s (WLED stays in realtime mode, does not jump to its own
    // preset), then stop. The fade goes through out_leds, so when the TV
    // comes back, the rise limit lights the strip up smoothly from there.
    std::vector<color_pipeline::RGB8> held_leds;
    std::thread out_thread;
    if (svc) out_thread = std::thread([&] {
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            std::lock_guard<std::mutex> lk(frame_mu);
            auto now = std::chrono::steady_clock::now();
            if (!have_hue_frame || out_leds.empty()) continue;
            const double gap_ms = std::chrono::duration<double, std::milli>(now - last_hue_frame).count();
            if (out_phase == "live") {
                if (gap_ms < 150.0) continue;  // frames are flowing (25 fps = 40 ms)
                held_leds = out_leds;
                out_phase = "hold";
            }
            if (out_phase == "stopped") continue;
            color_pipeline::Settings s = strip_settings.get();
            color_pipeline::LossOutput lo = color_pipeline::stream_loss_output(s, gap_ms);
            std::string phase = !lo.send ? "stopped" : lo.factor >= 1.0 ? "hold" : lo.factor > 0.0 ? "fade" : "black";
            if (phase != out_phase) {
                if (phase == "fade")
                    std::fprintf(stderr, "[out] no Hue frames for %.1f s: fading the strip to black (%.1f s)\n",
                                 gap_ms / 1000.0, s.loss_fade_ms / 1000.0);
                else if (phase == "black" && out_phase == "hold")
                    std::fprintf(stderr, "[out] no Hue frames for %.1f s: strip black\n", gap_ms / 1000.0);
                else if (phase == "stopped")
                    std::fprintf(stderr, "[out] no Hue frames for %.0f s: stopped sending (WLED goes back to its "
                                         "own preset - set it to off)\n", gap_ms / 1000.0);
                out_phase = phase;
            }
            const bool every_tick = phase == "fade";
            const double since_send = std::chrono::duration<double, std::milli>(now - last_wled_send).count();
            if (phase != "stopped" && !every_tick && since_send < 500.0) continue;
            // v0.33: fit_budget - the held frame obeys the budget of NOW (it may have been lowered meanwhile)
            if (phase != "stopped") out_leds = color_pipeline::fit_budget(color_pipeline::scale_frame(held_leds, lo.factor), s);
            last_out_time = now;
            last_ma = color_pipeline::estimate_current_ma(out_leds, s);
            // (Not added to strip_stats: that log is "while the TV streams";
            // the hold + fade are ~2 s of light per stop.)
            monitor_state::StripInfo si = last_si;
            si.output_phase = phase;
            si.limit_mode = "none";
            si.peak_leds.clear();
            si.target_ma = last_ma;
            monitor_state_obj.update_strip(out_leds, si);
            if (phase != "stopped" && wled_sender.is_open() && !output_closed) {
                bool sent = wled_sender.send(out_leds);
                monitor_state_obj.note_wled_send(sent, wled_sender.last_error());
            }
            last_wled_send = now;
        }
    });
    if (svc) out_thread.detach();

    std::atomic<bool> dtls_stop{false};
    std::atomic<bool> dtls_in_session{false};
    std::thread dtls_thread;
    if (svc) dtls_thread = std::thread([&] {
        dtls_psk::Options dtls_opts;
        dtls_opts.port = stream_port;
        dtls_opts.stop = &dtls_stop;
        dtls_opts.in_session = &dtls_in_session;
        dtls_opts.drop_session = &dtls_drop;
        dtls_opts.on_handshake = [&monitor_state_obj](bool ok, const std::string& ip, const std::string& detail) {
            const double latency = monitor_state_obj.note_handshake(ok, detail);
            if (ok && latency >= 0) {
                std::fprintf(stderr, "[main] DTLS handshake %.2f s after stream.active=true (the TV allows %.0f s)%s\n",
                             latency, monitor_state::SharedState::tv_dtls_limit_s,
                             latency > monitor_state::SharedState::tv_dtls_limit_s ? " - TOO LATE, the TV has probably given up" : "");
            } else if (!ok) {
                std::fprintf(stderr, "[main] DTLS handshake from %s FAILED (%s). If the TV had asked for the stream it is "
                                     "now locked to LightGroup until the TV is restarted.\n", ip.c_str(), detail.c_str());
            }
        };
        // v0.29: the TV's session survives q/s, r r, ambi restart (per identity)
        dtls_opts.session_file = dtls_session_path;
        dtls_opts.on_resumed = [&] {
            monitor_state_obj.set_stream_active(true);
            if (!resume_group.empty()) resume_ready = true;  // REST applies it on its next request
        };
        dtls_opts.on_old_session = [&monitor_state_obj](const std::string& ip, uint64_t packets, bool active) {
            monitor_state_obj.set_old_session(ip, packets, active);
        };
        dtls_psk::run_server(
            dtls_opts,
            [&pairing](const std::string& username) { return pairing.psk_for(username); },
            on_frame);
    });
    if (svc) dtls_thread.detach();

    // v0.31: watches the TV's 3 s limit between stream.active=true and the
    // DTLS handshake. Runs for the life of the process (like the DTLS thread).
    if (svc) std::thread([&monitor_state_obj] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (monitor_state_obj.check_dtls_deadline()) {
                std::fprintf(stderr, "[main] no DTLS handshake within %.0f s of stream.active=true: the TV has locked "
                                     "itself to LightGroup (no stream) - it needs a TV RESTART to try again\n",
                             monitor_state::SharedState::tv_dtls_limit_s);
            }
        }
    }).detach();

    if (svc) {
        std::fprintf(stderr, "ready: REST on :%u, DTLS-PSK Entertainment stream on udp:%u, SSDP on udp:1900\n",
                     rest_port, stream_port);
    } else {
        std::fprintf(stderr, "SERVICE STOPPED: no REST, DTLS, SSDP/mDNS, JointSpace or LED output - only the "
                             "terminal and the monitor page. s (or Start on the page) starts it.\n");
    }
    std::fprintf(stderr, "monitor dashboard: http://%s:%u/\n", ip.c_str(), monitor_port);
    std::fprintf(stderr, "point the TV's Ambilight+hue wizard at this bridge; q stops the service, s starts it, "
                         "r twice restarts, x or Ctrl+C exits, p opens pairing for 30 s.\n\n");

    // ---- fixed terminal dashboard (optional) ---------------------------
    // Only now, right before the main loop - everything printed above is
    // the one-time startup banner, meant to be read once and scroll by
    // normally, same as the old Python prototype's own plain prints before
    // its DASHBOARD object existed. From here on, on a real terminal,
    // every further line (including the dozens of existing
    // fprintf(stderr, ...) calls scattered across the other source files -
    // none of them need to change) is redirected into the dashboard's
    // rolling EVENTS box instead of scrolling the screen forever.
    std::unique_ptr<terminal_dashboard::TerminalDashboard> dashboard;
    int saved_stderr_fd = -1;
    if (want_dashboard) {
        dashboard = std::make_unique<terminal_dashboard::TerminalDashboard>(
            "http://" + ip + ":" + std::to_string(monitor_port) + "/", AMBI_BRIDGE_VERSION);
        dashboard->set_service_stopped(g_service_stopped);
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            saved_stderr_fd = dup(STDERR_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            int read_fd = pipefd[0];
            terminal_dashboard::TerminalDashboard* dash_ptr = dashboard.get();
            // The dashboard only shows the last 2 log lines, which on its own
            // made it impossible to see what the TV was actually doing
            // (v5-v7). Every line also goes to <state-dir>/ambi_bridge.log,
            // timestamped, so the full sequence is always there to read
            // (sudo tail -f /var/lib/ambi-bridge-cpp/ambi_bridge.log).
            // Started fresh each run; capped at ~5 MB, then truncated.
            // v0.22: the previous run's log is kept as ambi_bridge.log.1
            // (one generation), so what the TV did before a restart is not
            // lost - needed to catch the "Immersive greyed out" sequence.
            // v0.29: three old runs (.1 newest .. .3), since q then s = two runs
            std::string log_path = state_dir + "/ambi_bridge.log";
            std::rename((log_path + ".2").c_str(), (log_path + ".3").c_str());
            std::rename((log_path + ".1").c_str(), (log_path + ".2").c_str());
            std::rename(log_path.c_str(), (log_path + ".1").c_str());
            std::thread stderr_reader([read_fd, dash_ptr, log_path] {
                FILE* log = std::fopen(log_path.c_str(), "w");
                long written = 0;
                if (log) {
                    // log lines only carry HH:MM:SS, so say which day this run began
                    std::time_t t0 = std::time(nullptr);
                    std::tm tm0{};
                    localtime_r(&t0, &tm0);
                    char d0[40];
                    std::strftime(d0, sizeof(d0), "%Y-%m-%d %H:%M:%S", &tm0);
                    std::fprintf(log, "=== log started %s (ambi-bridge-cpp %s) ===\n", d0, AMBI_BRIDGE_VERSION);
                    std::fflush(log);
                }
                std::string buf;
                char chunk[1024];
                for (;;) {
                    ssize_t n = read(read_fd, chunk, sizeof(chunk));
                    if (n <= 0) break;  // write end closed (process exiting)
                    buf.append(chunk, static_cast<size_t>(n));
                    size_t pos;
                    while ((pos = buf.find('\n')) != std::string::npos) {
                        std::string line = buf.substr(0, pos);
                        buf.erase(0, pos + 1);
                        if (line.empty()) continue;
                        dash_ptr->notice(line);
                        if (log) {
                            if (written > 5L * 1024 * 1024) {
                                log = std::freopen(log_path.c_str(), "w", log);
                                written = 0;
                                if (!log) continue;
                            }
                            auto now = std::chrono::system_clock::now();
                            std::time_t tt = std::chrono::system_clock::to_time_t(now);
                            int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          now.time_since_epoch()).count() % 1000);
                            std::tm tm{};
                            localtime_r(&tt, &tm);
                            char ts[32];
                            std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
                            int w = std::fprintf(log, "%s.%03d %s\n", ts, ms, line.c_str());
                            if (w > 0) written += w;
                            std::fflush(log);
                        }
                    }
                }
                if (log) std::fclose(log);
            });
            stderr_reader.detach();
            dashboard->start();
        } else {
            dashboard.reset();  // pipe() failed - fall back to plain scrolling output
            want_dashboard = false;
            want_preview = console_mode != "quiet";
        }
    }
    std::thread dashboard_thread;
    if (dashboard) {
        terminal_dashboard::TerminalDashboard* dash_ptr = dashboard.get();
        dashboard_thread = std::thread([dash_ptr, &monitor_state_obj, &pairing, pairing_needs_button] {
            while (g_running.load()) {
                long link_ms = pairing_needs_button ? pairing.link_remaining_ms() : -1;
                bool tv_waiting = pairing_needs_button && pairing.pair_request_state().tv_waiting;
                dash_ptr->render(monitor_state_obj.get(), /*stream_configured=*/true, link_ms, tv_waiting);
                // 1 s (v9; was 0.5 s): calmer to look at, the web page is
                // the place for smooth live colours. v0.17: a key (t,
                // arrows) repaints at once instead of waiting out the second.
                for (int i = 0; i < 20 && g_running.load(); ++i) {
                    if (dash_ptr->take_redraw()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        });
    }

    // ---- keys (v9): 'p' = virtual link button, 'q' = stop -------------
    // Only when stdin is a real terminal (not under systemd). With the
    // dashboard on, stdin goes unbuffered/no-echo so a single 'p' works
    // without Enter and doesn't scribble on the screen; ISIG stays on, so
    // Ctrl+C still arrives as SIGINT. In plain mode 'p' + Enter works.
    if (::isatty(STDIN_FILENO)) {
        if (dashboard && tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
            termios raw = g_saved_termios;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) g_termios_saved = 1;
        }
        terminal_dashboard::TerminalDashboard* key_dash = dashboard.get();
        std::thread key_thread([&pairing, key_dash] {
            char c;
            auto last_r = std::chrono::steady_clock::now() - std::chrono::hours(1);
            // v0.17: arrow/page keys arrive as ESC [ A / ESC [ B / ESC [ 5 ~ /
            // ESC [ 6 ~ / ESC [ F or ESC [ 4 ~ (End); also ESC O A/B/F.
            int esc = 0;          // 0 none, 1 got ESC, 2 got ESC [ or ESC O
            std::string params;   // digits between '[' and the final byte
            unsigned char utf8_lead = 0;  // v0.26: Greek keyboard layout (UTF-8 CE xx / CF xx)
            while (g_running.load() && read(STDIN_FILENO, &c, 1) == 1) {
                if (esc == 0) {
                    const unsigned char u = static_cast<unsigned char>(c);
                    if (utf8_lead) {
                        // same physical key as the Latin letter: ρ r, π p, τ t, ε e (and capitals)
                        const int cp = ((utf8_lead & 0x1F) << 6) | (u & 0x3F);
                        utf8_lead = 0;
                        switch (cp) {
                            case 0x3C1: c = 'r'; break;  case 0x3A1: c = 'R'; break;
                            case 0x3C0: c = 'p'; break;  case 0x3A0: c = 'P'; break;
                            case 0x3C4: c = 't'; break;  case 0x3A4: c = 'T'; break;
                            case 0x3B5: c = 'e'; break;  case 0x395: c = 'E'; break;
                            case 0x3C3: c = 's'; break;  case 0x3A3: c = 'S'; break;  // v0.28: σ Σ
                            case 0x3C7: c = 'x'; break;  case 0x3A7: c = 'X'; break;  // v0.28: χ Χ
                            default: continue;
                        }
                    } else if (u == 0xCE || u == 0xCF) {
                        utf8_lead = u;
                        continue;
                    } else if (c == ';' || c == ':') {
                        c = 'q';  // the Q key on the Greek layout
                    }
                }
                if (esc == 1) {
                    esc = (c == '[' || c == 'O') ? 2 : 0;
                    params.clear();
                    continue;
                }
                if (esc == 2) {
                    if (c >= '0' && c <= '9') {
                        if (params.size() < 4) params.push_back(c);
                        continue;
                    }
                    esc = 0;
                    if (!key_dash) continue;
                    if (c == 'A') key_dash->scroll_log(1);
                    else if (c == 'B') key_dash->scroll_log(-1);
                    else if (c == 'F') key_dash->scroll_log_newest();
                    else if (c == '~' && params == "5") key_dash->scroll_log_pages(1);
                    else if (c == '~' && params == "6") key_dash->scroll_log_pages(-1);
                    else if (c == '~' && (params == "4" || params == "8")) key_dash->scroll_log_newest();
                    continue;
                }
                if (c == 0x1b) {
                    esc = 1;
                    continue;
                }
                // v0.28: q = stop the service (terminal + page stay), s = start,
                // x = leave the program (as Ctrl+C). Until v0.27 q left the program.
                if (c == 'q' || c == 'Q' || c == 's' || c == 'S') {
                    const bool stop = c == 'q' || c == 'Q';
                    std::string err = request_service(stop ? "stop" : "start", "terminal");
                    if (err.empty()) break;
                    if (err == "already_stopped")
                        std::fprintf(stderr, "[console] the service is already stopped - s starts it, x exits\n");
                    else if (err == "already_running")
                        std::fprintf(stderr, "[console] the service is already running - q stops it, x exits\n");
                    else
                        std::fprintf(stderr, "[console] %s not done (%s)\n", stop ? "stop" : "start", err.c_str());
                    continue;
                }
                if (c == 'x' || c == 'X') {
                    request_service("exit", "terminal");
                    break;
                }
                if (c == 'r' || c == 'R') {
                    // v0.24: twice within 3 s = restart (one stray key must not do it)
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_r <= std::chrono::seconds(3)) {
                        last_r = now - std::chrono::hours(1);
                        std::string err = request_service("restart", "terminal");
                        if (!err.empty()) std::fprintf(stderr, "[console] restart not done (%s)\n", err.c_str());
                        else break;
                    } else {
                        last_r = now;
                        std::fprintf(stderr, "[console] press r again within 3 s to restart the bridge\n");
                    }
                }
                if ((c == 'p' || c == 'P') && g_service_stopped) {
                    std::fprintf(stderr, "[console] the service is stopped - press s to start it, then p\n");
                } else if (c == 'p' || c == 'P') {
                    pairing.press_link_button(std::chrono::seconds(30));
                    std::fprintf(stderr, "[console] link button pressed - pairing open for 30 s\n");
                }
                if (key_dash && (c == 't' || c == 'T')) key_dash->toggle_tv();
                if (key_dash && (c == 'e' || c == 'E')) key_dash->scroll_log_newest();
            }
        });
        key_thread.detach();  // blocked in read() at exit; _Exit below ends it
    }

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    // v0.24 (review item 5): end a running stream session with close_notify,
    // so the TV knows it is over (instead of streaming on into a session the
    // next start cannot decrypt). The read loop sees the flag on its next
    // frame (40 ms at 25 fps) or read timeout (2 s when the TV is quiet).
    dtls_stop = true;
    // v0.33: Stop (q / the page) and Exit (x, Ctrl+C, SIGTERM) leave the strip
    // DARK: one black frame, sent three times (UDP), and nothing after it.
    // Until v0.32 the last picture stayed lit for WLED's 2.5 s timeout. NOT on
    // a restart: the stream goes on in the next run a moment later (v0.29) and
    // a black frame would only make the strip blink. Only when this run really
    // drove the strip. After WLED's 2.5 s it still goes to ITS OWN preset - set
    // that preset to "off" (README "WLED output"); the bridge cannot do that.
    {
        const bool restart_only = g_restart.load() && !g_exec_stopped.load();
        bool send_black = false;
        std::vector<color_pipeline::RGB8> black;
        {
            std::lock_guard<std::mutex> lk(frame_mu);
            send_black = svc && !restart_only && wled_sender.is_open() && have_hue_frame && !out_leds.empty() &&
                         out_phase != "stopped";
            if (send_black) {
                black.assign(out_leds.size(), color_pipeline::RGB8{0, 0, 0});
                out_leds = black;
                output_closed = true;
            }
        }
        if (send_black) {
            for (int i = 0; i < 3; ++i) {
                wled_sender.send(black);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            std::fprintf(stderr, "[out] %s: strip set to black (3 DDP frames)\n", g_exec_stopped.load() ? "stop" : "exit");
        }
    }
    for (int i = 0; i < 25 && dtls_in_session.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // v0.29: with a saved session, keep which group was streaming (see above)
    if (svc) {
        std::lock_guard<std::mutex> lk(stream_mu);
        if (access(dtls_session_path.c_str(), F_OK) == 0 && !stream_group.empty()) {
            std::ofstream wf(stream_resume_path, std::ios::trunc);
            wf << stream_group << "\n" << stream_owner << "\n";
        }
    } else if (!resume_group.empty()) {
        // stopped run: hand the previous run's group on to the next start
        std::ofstream wf(stream_resume_path, std::ios::trunc);
        wf << resume_group << "\n" << resume_owner << "\n";
    }
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
        g_termios_saved = 0;
    }
    if (dashboard_thread.joinable()) dashboard_thread.join();
    if (dashboard) {
        dashboard->stop();
        if (saved_stderr_fd >= 0) {
            dup2(saved_stderr_fd, STDERR_FILENO);
            close(saved_stderr_fd);
        }
    }
    std::fprintf(stderr, "\nshutting down.\n");
    strip_stats_obj.save();  // v0.23: keep the per-LED totals of this run
    // Withdraw the mDNS record cleanly (SIGTERM the avahi-publish-*
    // children, see mdns_ssdp.hpp) so the *.local hostname is free again
    // immediately, instead of lingering and colliding with the next start.
    mdns_ssdp::stop_mdns();
    // Exit here rather than `return 0`. Returning from main destroys main's
    // local objects (js_settings, monitor_state_obj, pairing, ...) while
    // the detached server/poller threads are still running and using
    // them. Confirmed with gdb on a real hang: the main thread blocked
    // forever in pthread_cond_destroy() inside jointspace::Settings'
    // destructor, because the JointSpace poll thread was still waiting on
    // that same condition variable - "shutting down." printed, then the
    // process never exited and the terminal never got its prompt back.
    // Everything that needs cleaning up (mDNS children, terminal state)
    // has been done above; the OS reclaims the rest.
    std::fflush(nullptr);
    if (g_restart.load()) {
        std::string exe;
        {
            std::lock_guard<std::mutex> lk(g_restart_mu);
            exe = g_restart_exe;
        }
        // Every socket/file of this run must go, or the new image inherits
        // ports 80/2100/8098/1900 and cannot bind them. 0-2 (the terminal) stay.
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
        for (long fd = 3; fd < maxfd; ++fd) close(static_cast<int>(fd));
        // v0.28: the same arguments, with --service-stopped only for Stop
        std::vector<char*> args;
        for (int i = 0; i < argc; ++i)
            if (std::strcmp(argv[i], "--service-stopped") != 0) args.push_back(argv[i]);
        static char stopped_flag[] = "--service-stopped";
        if (g_exec_stopped.load()) args.push_back(stopped_flag);
        args.push_back(nullptr);
        std::fprintf(stderr, "%s: %s\n\n",
                     g_exec_stopped.load() ? "stopping the service" : g_service_stopped ? "starting" : "restarting",
                     exe.c_str());
        std::fflush(nullptr);
        execv(exe.c_str(), args.data());
        std::fprintf(stderr, "restart: execv %s failed (%s) - starting this same build again\n", exe.c_str(),
                     std::strerror(errno));
        execv("/proc/self/exe", args.data());
        std::fprintf(stderr, "restart failed: %s\n", std::strerror(errno));
        std::_Exit(1);
    }
    std::_Exit(0);
}
