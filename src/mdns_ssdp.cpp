#include "mdns_ssdp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

namespace mdns_ssdp {

namespace {

// Pure PATH scan via access() - no fork(), no shell. This replaces an
// earlier version that ran `command -v <name>` through std::system(), which
// internally does its own fork()+exec("/bin/sh")+wait(). That matters here
// specifically because start_mdns() (the only caller of this function) is
// invoked from main() *after* ssdp_thread is already running (see
// main.cpp) - i.e. this process is already multithreaded every single time
// this runs, not just on the stale-process-cleanup path. Forking from a
// multithreaded process is a well-known hazard around the malloc heap lock:
// if another thread held it at the instant of fork(), the forked child can
// inherit it permanently locked, and anything the child does with the
// allocator before it execs (flushing C++ iostream buffers, building argv,
// std::string allocation, etc.) can corrupt or hang on it. The same
// reasoning that moved the stale-process pkill off std::system() (see
// run_and_wait_quiet() below) applies here, and this call site runs earlier
// and unconditionally, so it's the more likely actual source of the
// "free(): invalid pointer" crash seen on real hardware even after that
// first fix - this one was missed because it doesn't look like a process
// spawn from the call site, just a boolean helper.
bool binary_exists(const char* name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return false;
    std::string path(path_env);
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = (colon == std::string::npos) ? path.substr(start) : path.substr(start, colon - start);
        if (dir.empty()) dir = ".";
        std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return true;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

// PIDs of our own avahi-publish-* children, so stop_mdns() can SIGTERM
// them at shutdown. Plain file-local state is enough here: start_mdns()/
// stop_mdns() are each called exactly once, from main(), never concurrently
// with each other.
std::vector<pid_t> g_mdns_child_pids;

// Fire-and-forget: fork+exec, don't wait synchronously. Returns the child
// pid (or -1 on fork() failure) so the caller can track it for stop_mdns().
// The child's own exit (if the avahi daemon isn't running, say) just means
// no mDNS, not a crash here.
pid_t spawn_detached(const std::vector<std::string>& argv) {
    {
        std::string joined;
        for (auto& a : argv) { joined += "["; joined += a; joined += "] "; }
        std::fprintf(stderr, "[mdns] spawning: %s\n", joined.c_str());
    }
    // The child's stdout/stderr go into a pipe that a small thread here
    // reads and re-logs through our own stderr, instead of the child
    // inheriting the terminal directly. Until v7 the avahi-publish-*
    // children wrote "Established under name ..." / "Failed to add
    // address: Local name collision" straight onto the terminal at
    // whatever moment avahi got round to it - often after the terminal
    // dashboard had taken over the screen, landing in the middle of it.
    // Re-logged through stderr, those lines show up in the dashboard's
    // EVENTS box (or the plain log) like every other message.
    int out_pipe[2] = {-1, -1};
    bool have_pipe = pipe(out_pipe) == 0;
    pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "[mdns] fork() failed: %s\n", std::strerror(errno));
        if (have_pipe) {
            close(out_pipe[0]);
            close(out_pipe[1]);
        }
        return -1;
    }
    if (pid == 0) {
        if (have_pipe) {
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(out_pipe[1], STDERR_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
        }
        std::vector<char*> cargs;
        cargs.reserve(argv.size() + 1);
        for (auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        execvp(cargs[0], cargs.data());
        _exit(127);  // only reached if execvp itself failed
    }
    if (have_pipe) {
        close(out_pipe[1]);
        int rfd = out_pipe[0];
        std::string who = argv.empty() ? "avahi" : argv[0];
        std::thread([rfd, who] {
            std::string buf;
            char chunk[512];
            for (;;) {
                ssize_t n = read(rfd, chunk, sizeof(chunk));
                if (n <= 0) break;  // child exited (or was stopped by stop_mdns)
                buf.append(chunk, static_cast<size_t>(n));
                size_t pos;
                while ((pos = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, pos);
                    buf.erase(0, pos + 1);
                    if (!line.empty()) std::fprintf(stderr, "[mdns] %s: %s\n", who.c_str(), line.c_str());
                }
            }
            close(rfd);
        }).detach();
    }
    // Parent: not waiting synchronously here - stop_mdns() does that at
    // shutdown (SIGTERM, then a bounded waitpid so avahi can withdraw the
    // record cleanly before we reap it). See header comment.
    return pid;
}

// Runs `argv` directly via fork()+execvp() (no shell), discarding its
// stdout/stderr, and waits for it to exit. Used for the one-shot stale-
// process cleanup below, which must *finish* before we continue - unlike
// spawn_detached(), which is fire-and-forget.
//
// Deliberately not std::system(): by the time start_mdns() runs, this
// process already has another thread alive (ssdp_thread - see main.cpp),
// and system() does its own fork()+exec("/bin/sh", ...)+wait() internally.
// fork() from a multithreaded program is a well-known hazard around the
// malloc heap lock - if another thread held it at the instant of fork(),
// the child inherits it permanently locked, which can show up as exactly
// this kind of heap corruption ("free(): invalid pointer") the next time
// the child's single surviving thread touches the allocator. This keeps
// the one extra fork this cleanup step needs as plain as the children
// spawn_detached() already creates, instead of adding system()'s internal
// fork+shell+wait on top of those.
void run_and_wait_quiet(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        std::vector<char*> cargs;
        cargs.reserve(argv.size() + 1);
        for (auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        execvp(cargs[0], cargs.data());
        _exit(127);
    }
    waitpid(pid, nullptr, 0);
}

}  // namespace

void start_mdns(const bridge_identity::Identity& identity) {
    if (!binary_exists("avahi-publish-service")) {
        std::fprintf(stderr,
                     "[mdns] avahi-publish-service not found (apt install avahi-utils) "
                     "-> mDNS disabled. The TV may not find the bridge without it; "
                     "SSDP discovery still runs as a fallback.\n");
        return;
    }
    std::string host = "philips-hue-" + identity.bridgeid.substr(identity.bridgeid.size() - 6) + ".local";
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });

    // Defensive cleanup for a previous *unclean* exit of this same program
    // (kill -9, crash, power loss) that left an avahi-publish-address/
    // -service process holding this exact hostname - the symptom is avahi
    // logging "Failed to add address: Local name collision" on our next
    // start. A normal exit doesn't need this (stop_mdns() already killed
    // them), so this is purely a safety net for an unclean one. Matched on
    // our own exact hostname, not a bare "avahi-publish", so this can't
    // step on some *other* avahi-publish-* use on a shared machine.
    if (binary_exists("pkill")) {
        run_and_wait_quiet({"pkill", "-f", "avahi-publish-(address|service).*" + host});
        // Give avahi-daemon a moment to actually process that process's
        // death and withdraw its record over D-Bus before we try to
        // register the same name again - pkill returning only means the
        // signal was delivered, not that avahi has caught up yet, and
        // racing that is a second plausible way to hit the same
        // "Local name collision" this cleanup exists to prevent.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // avahi-publish-address takes <host-name> <address> positionally - NOT
    // -H, which is a avahi-publish-service-only flag (confirmed against
    // the real --help text and a live invocation in the sandbox this was
    // built in: "-H" here produced "Bad number of arguments").
    pid_t addr_pid = spawn_detached({"avahi-publish-address", "-R", host, identity.ip});
    if (addr_pid > 0) g_mdns_child_pids.push_back(addr_pid);

    std::string service_name = "Philips Hue - " + identity.bridgeid.substr(identity.bridgeid.size() - 6);
    std::string bridgeid_lower = identity.bridgeid;
    std::transform(bridgeid_lower.begin(), bridgeid_lower.end(), bridgeid_lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    pid_t svc_pid = spawn_detached({"avahi-publish-service", "-H", host, service_name, "_hue._tcp",
                     std::to_string(identity.port), "bridgeid=" + bridgeid_lower, "modelid=BSB002"});
    if (svc_pid > 0) g_mdns_child_pids.push_back(svc_pid);

    std::fprintf(stderr, "[mdns] announcing _hue._tcp as \"%s\" (host %s) via avahi\n",
                 service_name.c_str(), host.c_str());
}

void stop_mdns() {
    if (g_mdns_child_pids.empty()) return;
    for (pid_t pid : g_mdns_child_pids) kill(pid, SIGTERM);
    // Bounded wait, not a blocking waitpid(): avahi-publish-* exit promptly
    // on SIGTERM in practice, but this must not hang program shutdown if
    // one of them somehow doesn't. Poll non-blockingly for up to ~1s total,
    // then give up on reaping (process exit cleans up any that are left;
    // only avahi's own record-withdrawal, which already happened on
    // SIGTERM delivery, was the actual goal here).
    for (int i = 0; i < 20; ++i) {
        bool any_left = false;
        for (pid_t pid : g_mdns_child_pids) {
            if (pid <= 0) continue;
            if (waitpid(pid, nullptr, WNOHANG) == 0) any_left = true;
        }
        if (!any_left) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    g_mdns_child_pids.clear();
}

void run_ssdp_responder(const bridge_identity::Identity& identity, discovery::Registry* discovery) {
    const char* group = "239.255.255.250";
    const int port = 1900;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        std::fprintf(stderr, "[ssdp] socket() failed: %s\n", std::strerror(errno));
        return;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[ssdp] cannot bind :1900 (%s); SSDP disabled\n", std::strerror(errno));
        close(sock);
        return;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(group);
    mreq.imr_interface.s_addr = inet_addr(identity.ip.c_str());
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        std::fprintf(stderr, "[ssdp] IP_ADD_MEMBERSHIP failed: %s (M-SEARCH replies will still work; "
                              "periodic NOTIFY may not reach all listeners)\n", std::strerror(errno));
    }

    std::string location = "http://" + identity.ip + ":" + std::to_string(identity.port) + "/description.xml";
    std::string header = "HTTP/1.1 200 OK\r\nHOST: 239.255.255.250:1900\r\nEXT:\r\nCACHE-CONTROL: max-age=100\r\n"
                          "LOCATION: " + location + "\r\n"
                          "SERVER: Linux/3.14.0 UPnP/1.0 IpBridge/1.20.0\r\n"
                          "hue-bridgeid: " + identity.bridgeid + "\r\n";
    std::string udn = "uuid:" + identity.udn;
    std::vector<std::pair<std::string, std::string>> variants = {
        {"upnp:rootdevice", udn + "::upnp:rootdevice"},
        {udn, udn},
        {"urn:schemas-upnp-org:device:basic:1", udn},
    };

    // Periodic NOTIFY on a separate outbound socket, same as fakebridge.py's notify_loop.
    std::thread notify_thread([&identity, group, header, udn, variants] {
        int out = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (out < 0) return;
        int ttl = 2;
        setsockopt(out, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        in_addr ifaddr{};
        ifaddr.s_addr = inet_addr(identity.ip.c_str());
        setsockopt(out, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(1900);
        dst.sin_addr.s_addr = inet_addr(group);

        for (;;) {
            for (auto& [nt, usn] : variants) {
                std::ostringstream msg;
                msg << "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=100\r\n"
                    << "LOCATION: http://" << identity.ip << ":" << identity.port << "/description.xml\r\n"
                    << "SERVER: Linux/3.14.0 UPnP/1.0 IpBridge/1.20.0\r\nNTS: ssdp:alive\r\n"
                    << "hue-bridgeid: " << identity.bridgeid << "\r\nNT: " << nt << "\r\nUSN: " << usn << "\r\n\r\n";
                std::string m = msg.str();
                sendto(out, m.data(), m.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            }
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    });
    notify_thread.detach();

    std::fprintf(stderr, "[ssdp] responder running on :1900\n");
    char buf[2048];
    for (;;) {
        sockaddr_in src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&src), &src_len);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string msg(buf, static_cast<size_t>(n));
        if (msg.rfind("M-SEARCH", 0) == 0 && msg.find("ssdp:discover") != std::string::npos) {
            char src_ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
            // v0.17: also log what it searches for (ST) and who (USER-AGENT),
            // e.g. "urn:dial-multiscreen-org:service:dial:1" from Chromium =
            // a browser looking for cast devices, not for a Hue bridge.
            auto header_value = [&msg](const char* name) {
                std::string lower = msg;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
                std::string key = std::string("\r\n") + name + ":";
                size_t p = lower.find(key);
                if (p == std::string::npos) return std::string("-");
                p += key.size();
                size_t e = msg.find("\r\n", p);
                std::string v = msg.substr(p, e == std::string::npos ? std::string::npos : e - p);
                size_t s = v.find_first_not_of(' ');
                v = s == std::string::npos ? std::string() : v.substr(s);
                if (v.size() > 200) v.resize(200);
                for (char& c : v)
                    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
                return v.empty() ? std::string("-") : v;
            };
            std::string st = header_value("st"), ua = header_value("user-agent");
            std::vector<int> answer = discovery::answer_variants(st, identity.udn, discovery && discovery->hue_only());
            std::fprintf(stderr, "[ssdp] M-SEARCH from %s ST=%s UA=%s%s\n", src_ip, st.c_str(), ua.c_str(),
                         answer.empty() ? " (not a Hue search: ignored)" : "");
            if (discovery) discovery->note(src_ip, "ssdp", st, ua, !answer.empty(), discovery::now_ms());
            if (answer.empty()) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            for (int vi : answer) {
                const auto& [vst, usn] = variants[static_cast<size_t>(vi)];
                std::ostringstream resp;
                resp << header << "ST: " << vst << "\r\nUSN: " << usn << "\r\n\r\n";
                std::string r = resp.str();
                sendto(sock, r.data(), r.size(), 0, reinterpret_cast<sockaddr*>(&src), src_len);
            }
        }
    }
}

}  // namespace mdns_ssdp
