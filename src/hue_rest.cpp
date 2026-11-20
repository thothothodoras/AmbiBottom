#include "hue_rest.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

#include "hue_core.hpp"

// v0.32: the Hue REST logic itself (lights, groups, pairing replies) lives in
// shared/ambi_core/hue_core.cpp, the same code the ESP32 usermod runs. This
// file is only the Linux side of it: sockets, one thread per connection, the
// groups.json file, and the bridge's own /ambi/settings extension.
namespace hue_rest {

namespace {

// ---- minimal blocking HTTP/1.1 request parsing -----------------------------
struct HttpRequest {
    std::string method, path;
    std::string user_agent;  // v0.17: logged for GET /description.xml (who is probing us)
    std::string body;
    bool ok = false;
};

HttpRequest read_request(int fd) {
    HttpRequest req;
    std::string buf;
    char chunk[4096];
    size_t header_end;
    for (;;) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return req;  // closed or error before a full header arrived
        buf.append(chunk, static_cast<size_t>(n));
        header_end = buf.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (buf.size() > 65536) return req;  // runaway header, bail safely
    }
    std::string head = buf.substr(0, header_end);
    std::string rest = buf.substr(header_end + 4);

    std::istringstream hs(head);
    std::string request_line;
    std::getline(hs, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
    std::istringstream rl(request_line);
    std::string version;
    rl >> req.method >> req.path >> version;
    if (req.method.empty() || req.path.empty()) return req;

    size_t content_length = 0;
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        size_t vs = val.find_first_not_of(' ');
        if (vs != std::string::npos) val = val.substr(vs);
        if (key == "content-length") {
            try { content_length = static_cast<size_t>(std::stoul(val)); } catch (...) { content_length = 0; }
        } else if (key == "user-agent") {
            req.user_agent = val;
        }
    }

    while (rest.size() < content_length) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return req;  // peer closed mid-body
        rest.append(chunk, static_cast<size_t>(n));
    }
    req.body = rest.substr(0, content_length);
    req.ok = true;
    return req;
}

void send_response(int fd, int code, const std::string& body, const std::string& content_type) {
    const char* status_text = code == 200 ? "OK" : code == 404 ? "Not Found" : "Error";
    std::ostringstream resp;
    // "Server: nginx" + Date, as a real Hue bridge and the old Python bridge
    // (BaseHTTPRequestHandler with server_version="nginx") both send.
    char date[64];
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&t, &tm);
        std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    }
    resp << "HTTP/1.1 " << code << " " << status_text << "\r\n"
         << "Server: nginx\r\n"
         << "Date: " << date << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Connection: close\r\n\r\n"
         << body;
    std::string out = resp.str();
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

// Takes ownership of `obj`: prints it, frees it, sends it.
void send_json(int fd, int code, cJSON* obj) {
    char* text = cJSON_PrintUnformatted(obj);
    std::string body = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(obj);
    send_response(fd, code, body, "application/json");
}

// Applies "on"/"bri"/"xy" keys from a Hue state/action body onto one Light,
// returning the per-key {"success": {addr: value}} entries fakebridge.py's
// _light_state() builds. Does not take ownership of `body`.

cJSON* make_error(const std::string& path, int type, const std::string& desc) {
    cJSON* arr = cJSON_CreateArray();
    cJSON* wrap = cJSON_CreateObject();
    cJSON* err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "type", type);
    cJSON_AddStringToObject(err, "address", path.c_str());
    cJSON_AddStringToObject(err, "description", desc.c_str());
    cJSON_AddItemToObject(wrap, "error", err);
    cJSON_AddItemToArray(arr, wrap);
    return arr;
}

std::string read_file(const std::string& path) {
    if (path.empty()) return {};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > (1u << 20)) {  // not ours
            out.clear();
            break;
        }
    }
    std::fclose(f);
    return out;
}

// Same atomic write as before (tmp + rename).
void write_file_atomic(const std::string& path, const std::string& text) {
    if (path.empty()) return;
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (ok) std::rename(tmp.c_str(), path.c_str());
}

class PairingAdapter : public hue_core::Pairing {
public:
    explicit PairingAdapter(pairing_store::ThreadSafeStore& store) : store_(store) {}
    bool get(const std::string& username) override { return store_.get(username).has_value(); }
    std::pair<std::string, hue_core::PairedUser> pair(const std::string& devicetype, bool generate_clientkey) override {
        auto [username, user] = store_.pair(devicetype, generate_clientkey);
        return {username, hue_core::PairedUser{user.clientkey_hex}};
    }
    std::vector<std::pair<std::string, std::string>> users_with_devicetype() override {
        return store_.users_with_devicetype();
    }
    long link_remaining_ms() override { return store_.link_remaining_ms(); }
    void note_pair_refused() override { store_.note_pair_refused(); }

private:
    pairing_store::ThreadSafeStore& store_;
};

}  // namespace

void run_server(const Options& opts,
                 const bridge_identity::Identity& identity,
                 pairing_store::ThreadSafeStore& pairing,
                 const std::string& pairing_store_path,
                 const StreamActiveHandler& on_stream_active,
                 settings::SharedSettings* strip_settings,
                 discovery::Registry* discovery) {
    (void)pairing_store_path;  // ThreadSafeStore already persists its own writes
    const std::string groups_path = opts.groups_store_path;

    PairingAdapter pairing_adapter(pairing);
    hue_core::Options core_opts;
    core_opts.lamp_names = opts.lamp_names;
    core_opts.apiversion = opts.apiversion;
    core_opts.swversion = opts.swversion;
    core_opts.tv_ip = opts.tv_ip;
    core_opts.require_link_button = opts.require_link_button;
    core_opts.take_resumed_stream = opts.take_resumed_stream;
    core_opts.on_lightgroup_used = opts.on_lightgroup_used;
    core_opts.fail_lamp_check = opts.fail_lamp_check;  // v0.37
    core_opts.on_stream_active = on_stream_active;
    if (discovery) {
        core_opts.on_description = [discovery](const std::string& src, const std::string& ua) {
            discovery->note(src, "description", "-", ua, true, discovery::now_ms());
        };
    }
    if (!groups_path.empty()) {
        core_opts.load_groups_text = [groups_path] { return read_file(groups_path); };
        // The store as v0.30 left it is kept once, before the first change
        // v0.31+ makes to it (groups.json.pre-v031).
        core_opts.save_groups_text = [groups_path](const std::string& text) {
            const std::string keep = groups_path + ".pre-v031";
            const std::string before = read_file(groups_path);
            if (!before.empty() && before.find("\"_high_water\"") == std::string::npos && read_file(keep).empty()) {
                write_file_atomic(keep, before);
            }
            write_file_atomic(groups_path, text);
        };
    }
    hue_core::Bridge bridge(core_opts, identity, pairing_adapter);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::fprintf(stderr, "[rest] socket() failed: %s\n", std::strerror(errno));
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(opts.port);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[rest] bind 0.0.0.0:%u failed: %s\n", opts.port, std::strerror(errno));
        close(listen_fd);
        return;
    }
    if (listen(listen_fd, 16) != 0) {
        std::fprintf(stderr, "[rest] listen() failed: %s\n", std::strerror(errno));
        close(listen_fd);
        return;
    }
    std::fprintf(stderr, "[rest] listening on 0.0.0.0:%u\n", opts.port);

    // v0.31: one short-lived thread per connection. Until v0.30 this loop
    // read and answered one connection at a time, so a client that connected
    // and then said nothing held everyone else up for the 2 s receive timeout.
    // The TV decides "Entertainment or LightGroup" with 10 GET /lights/1 that
    // must each answer within 300 ms (HueControl.testHueConnection, 8 of 10
    // needed) - one slow stranger was enough to push it to LightGroup. Now the
    // slow part (reading the request) runs outside state_mu; lights/groups are
    // only touched while holding it.
    std::mutex state_mu;
    std::atomic<int> open_connections{0};
    constexpr int max_open_connections = 48;

    auto handle_connection = [&](int fd, const sockaddr_in& peer) {
        // This loop serves one connection at a time (it owns the lights/
        // groups state, so it stays single-threaded on purpose). Until v6
        // that recv() had no timeout, so any LAN device that opened a
        // connection and then went quiet stalled every request behind it -
        // including the TV's own. A short bound keeps one bad client from
        // ever holding the TV up for more than this.
        timeval tv{};
        tv.tv_sec = 2;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        char src_buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, src_buf, sizeof(src_buf));
        std::string src = src_buf;

        HttpRequest req = read_request(fd);
        if (!req.ok) {
            close(fd);
            return;
        }

        std::lock_guard<std::mutex> state_lock(state_mu);

        std::string path = req.path;
        size_t qpos = path.find('?');
        if (qpos != std::string::npos) path = path.substr(0, qpos);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        if (path.empty()) path = "/";

        // ---- /ambi/settings: this bridge's own small extension, not part
        // of the real Hue API. Since v0.15 (Phase 3) the place to change
        // these is the monitor page (local browser only, like the
        // JointSpace credentials); this port faces the TV and the LAN, so
        // here the route only answers requests from this machine itself
        // (127.0.0.1, e.g. curl on the Linux box). A browser page elsewhere
        // can't use it either: a cross-origin PUT needs a CORS preflight,
        // which this server never grants. Values go through the same
        // settings::merge_json() limits as settings.json. -----------------
        if (path == "/ambi/settings" && strip_settings) {
            cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
            std::fprintf(stderr, "[rest] %s %s %s (%zu-byte body)\n", src.c_str(), req.method.c_str(), path.c_str(),
                         req.body.size());
            if (src != "127.0.0.1") {
                std::fprintf(stderr, "[rest] %s %s /ambi/settings refused: only from this machine (127.0.0.1)\n",
                             src.c_str(), req.method.c_str());
                send_json(fd, 403, make_error(path, 1, "only from this machine (127.0.0.1); use the monitor page"));
            } else if (req.method == "GET") {
                send_json(fd, 200, settings::to_json(strip_settings->get()));
            } else if (req.method == "PUT" && body) {
                color_pipeline::Settings s = strip_settings->get();  // start from current, PUT is a partial merge
                settings::merge_json(s, body);
                bool saved = strip_settings->update(s);
                std::fprintf(stderr, "[rest] /ambi/settings updated (led_count=%d, saved=%s)\n",
                             s.led_count, saved ? "yes" : "no (disk write failed, kept in memory)");
                cJSON* c = settings::to_json(s);
                cJSON_AddBoolToObject(c, "saved_to_disk", saved);
                send_json(fd, 200, c);
            } else {
                send_json(fd, 400, make_error(path, 1, "bad request"));
            }
            if (body) cJSON_Delete(body);
            close(fd);
            return;
        }

        const hue_core::Response r = bridge.handle(req.method, req.path, req.body, src, req.user_agent);
        send_response(fd, r.status, r.body, r.content_type);
        close(fd);
    };

    for (;;) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) continue;
        if (open_connections.load() >= max_open_connections) {
            close(fd);  // flood guard; the TV never has more than a couple open
            continue;
        }
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        ++open_connections;
        std::thread([&handle_connection, &open_connections, fd, peer] {
            handle_connection(fd, peer);
            --open_connections;
        }).detach();  // run_server never returns once it is listening, so the captures outlive every thread
    }
}

}  // namespace hue_rest
