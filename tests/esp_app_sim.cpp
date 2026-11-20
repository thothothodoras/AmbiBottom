// Runs the ESP32 usermod's application code (firmware/wled-usermod/lib/
// AmbiBottomEsp: ambi_bottom::App + ambi_dtls) as a Linux process, so the
// same end-to-end checks used for the Linux bridge (curl + tools/sender) can
// be run against the firmware's logic without an ESP32.
//
// What stands in for the ESP32 here: a few-line HTTP server instead of
// AsyncWebServer, files in a directory instead of NVS, a thread instead of a
// FreeRTOS task, and the LED values are only kept for /sim/leds. No SSDP
// socket is opened (the texts are served at /sim/ssdp for inspection), so it
// never shows up on the LAN.
//
// usage: esp_app_sim <state dir> <http port> <stream port> [lamps 3|5]
// extra routes: POST /sim/press (the link button), GET /sim/status,
// GET /sim/leds, GET /sim/ssdp?st=..., POST /sim/strip (JSON StripConfig subset)
// v0.37: the page and its API as on the device: GET /ambi (AMBI_ESP_PAGE or the generated
// firmware/wled-usermod/AmbiBottom/ambi_page.html), /ambi/api/{state,service,pair,login,
// settings/strip,demo}, and a short stand-in for WLED's /json/eff and /json/fxdata.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "ambi_bottom_app.h"

namespace {

std::string g_dir;
std::mutex g_leds_mu;
std::vector<ambi_bottom::Rgb> g_leds;
bool g_released = false;

std::string g_page_path;
static std::atomic<size_t> g_heap_free{200000}, g_heap_block{100000};  // v0.47: fake heap
// v0.40: what the usermod's "ESP & WLED" settings would hold (WLED Length, pin, lamps, auto Start)
struct SimDevice {
    int length = 43, pin = 38, lamps = 3;
    bool auto_start = false;
    std::string name = "AmbiBottom", button = "wled";  // v0.41
    int tz = 3;
    bool ntp = false;
    std::string hue_only_ip;  // v0.42
    bool demo_enabled = true;  // v0.43 (the firmware's default is off; the tests use the demo)
};
SimDevice g_sim_dev;

// v0.42: the security card (WLED's own values do not exist here: PIN / OTA lock off, default AP password)
std::string sim_security_json(ambi_bottom::App& app) {
    return "{\"can_edit\":true,\"pin\":false,\"ota_lock\":false,\"ota_same_subnet\":true,\"arduino_ota\":false,"
           "\"ap_default_pass\":true,\"ap_behavior\":0,\"hue_only_ip\":\"" + g_sim_dev.hue_only_ip +
           "\",\"tv_ip_seen\":\"" + app.tv_ip_seen() + "\",\"js_saved\":false}";
}

const int kSimWledLength = 43;  // v0.37: WLED's Length in the simulator (all LEDs; led_count = the ones used)

// v0.37: a few of WLED's effect names (ids as in WLED 16) and their metadata, so the
// page's effect list can be tried without a device; "Matrix 2D" checks the 2D filter.
const char* kSimEffects = "[\"Solid\",\"Blink\",\"Breathe\",\"Wipe\",\"Wipe Random\",\"Random Colors\",\"Sweep\",\"Dynamic\",\"Colorloop\",\"Rainbow\",\"RSVD\",\"Matrix 2D\",\"Pacifica\"]";
const char* kSimFxData = "[\"\",\"!,Duty cycle;!,!;!;01\",\"!;!,!;!;01\",\"!,!;!,!;!\",\"!;;!\",\"!;;!\",\"!;!,!;!\",\"!,!;;!\",\"!,Saturation;;!;01\",\"!,Size;;!\",\"\",\"!;;!;2\",\"!,Angle;;!;;pal=51\"]";
const char* const kSimEffectNames[] = {"Solid", "Blink", "Breathe", "Wipe", "Wipe Random", "Random Colors", "Sweep",
                                       "Dynamic", "Colorloop", "Rainbow", "RSVD", "Matrix 2D", "Pacifica"};

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// What the usermod's loop() would report: while running with the demo on and no TV
// stream, a slow colour wave at the demo brightness stands in for WLED's effect.
ambi_bottom::PageInfo sim_page_info(ambi_bottom::App& app, const ambi_bottom::StripConfig& strip) {
    ambi_bottom::PageInfo info;
    info.version = "v0.37-alpha (simulator)";
    info.output_target = "simulator (no strip)";
    info.wled_led_count = kSimWledLength;
    info.output_fps = 0;
    const ambi_bottom::Demo d = app.demo();
    const ambi_bottom::Status st = app.status(now_s());
    info.demo_showing = app.running() && d.on && !st.live;
    info.demo_fx = d.fx >= 0 ? d.fx : 12;
    info.demo_fx_name = info.demo_fx < 13 ? kSimEffectNames[info.demo_fx] : "?";
    if (info.demo_showing) {
        const double t = now_s() * (0.2 + d.speed / 128.0);
        const double k = d.bri_pct / 100.0;
        double ma = 0;
        for (int i = 0; i < strip.led_count; ++i) {
            const double ph = t + i * 0.15;
            const auto ch = [&](double off) { return static_cast<uint8_t>(k * 127.5 * (1 + std::sin(ph + off))); };
            ambi_bottom::Rgb c{ch(0), ch(2.1), ch(4.2)};
            ma += (c.r + c.g + c.b) / 765.0 * strip.ma_per_led + 1;
            info.demo_leds.push_back(c);
        }
        info.demo_ma = ma;
    }
    return info;
}

std::string read_all(int fd) {
    std::string buf;
    char chunk[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return {};
        buf.append(chunk, static_cast<size_t>(n));
        header_end = buf.find("\r\n\r\n");
    }
    size_t content_length = 0;
    std::string lower = buf.substr(0, header_end);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t cl = lower.find("content-length:");
    if (cl != std::string::npos) content_length = static_cast<size_t>(std::atol(lower.c_str() + cl + 15));
    while (buf.size() < header_end + 4 + content_length) {
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
    }
    return buf;
}

void reply(int fd, int status, const std::string& type, const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 " << status << " OK\r\nContent-Type: " << type << "\r\nContent-Length: " << body.size()
      << "\r\nConnection: close\r\n\r\n" << body;
    const std::string out = o.str();
    send(fd, out.data(), out.size(), MSG_NOSIGNAL);
}

double json_number(const std::string& body, const char* key, double fallback) {
    const size_t p = body.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return fallback;
    const size_t c = body.find(':', p);
    return c == std::string::npos ? fallback : std::atof(body.c_str() + c + 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <state dir> <http port> <stream port> [lamps]\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    {
        const char* env = std::getenv("AMBI_ESP_PAGE");
        std::string src = __FILE__;
        src = src.substr(0, src.rfind('/') == std::string::npos ? 0 : src.rfind('/'));
        g_page_path = env && *env ? env : (src.empty() ? std::string(".") : src) + "/../firmware/wled-usermod/AmbiBottom/ambi_page.html";
    }
    const int http_port = std::atoi(argv[2]);

    ambi_bottom::Platform platform;
    platform.load = [](const char* key) {
        std::ifstream f(g_dir + "/" + key + ".nvs");
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    platform.save = [](const char* key, const std::string& value) {
        std::ofstream f(g_dir + "/" + key + ".nvs", std::ios::trunc);
        f << value;
        return static_cast<bool>(f);
    };
    platform.random = [](uint8_t* out, size_t len) {
        std::random_device rd;
        for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(rd());
    };
    // v0.41: the wear log as a file in the state dir; the wall clock as on Linux (localtime_r does the zone)
    platform.stats_load = [] {
        std::ifstream f(g_dir + "/ambi_stats.json");
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    platform.stats_save = [](const std::string& text) {
        std::ofstream f(g_dir + "/ambi_stats.json", std::ios::trunc);
        f << text;
        return static_cast<bool>(f);
    };
    platform.heap = [](size_t* f, size_t* b) {  // v0.47: a fake heap, set by /sim/heap?free=&block=
        *f = g_heap_free.load();
        *b = g_heap_block.load();
    };
    platform.local_epoch_s = [] {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    };
    ambi_bottom::Config config;
    config.ip = "127.0.0.1";
    config.http_port = static_cast<uint16_t>(http_port);
    config.stream_port = static_cast<uint16_t>(std::atoi(argv[3]));
    config.lamps = argc > 4 ? std::atoi(argv[4]) : 3;
    ambi_bottom::App app(platform, config);
    ambi_bottom::StripConfig strip;
    app.set_strip(strip);

    std::thread([&app] { app.run_dtls(); }).detach();
    std::thread([&app] { app.run_jointspace(); }).detach();  // v0.37
    std::thread([&app] {  // the usermod's loop(): every 20 ms
        std::vector<ambi_bottom::Rgb> leds;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            bool release = false;
            const bool show = app.poll_leds(now_s(), leds, &release);
            std::lock_guard<std::mutex> lk(g_leds_mu);
            if (show) {
                g_leds = leds;
                g_released = false;
            }
            if (release) g_released = true;
        }
    }).detach();

    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(http_port));
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 16) != 0) {
        std::fprintf(stderr, "[sim] cannot listen on 127.0.0.1:%d\n", http_port);
        return 1;
    }
    std::fprintf(stderr, "[sim] ESP app on http://127.0.0.1:%d, stream udp/%s, state in %s\n", http_port, argv[3], g_dir.c_str());
    for (;;) {
        const int fd = accept(ls, nullptr, nullptr);
        if (fd < 0) continue;
        std::thread([fd, &app, &strip] {
            const std::string raw = read_all(fd);
            std::istringstream first(raw.substr(0, raw.find("\r\n")));
            std::string method, target;
            first >> method >> target;
            const size_t he = raw.find("\r\n\r\n");
            const std::string body = he == std::string::npos ? std::string() : raw.substr(he + 4);
            std::string path = target.substr(0, target.find('?'));
            // v0.36: the device's own names too (/ambibottom/pair, /ambibottom/status, a
            // /json/info with a stand-in "freeheap"), so tools/esp_hue_diff.sh and
            // tools/esp_stream_test.sh can be run against this simulator first.
            if (path == "/ambibottom/pair") path = "/sim/press";
            if (path == "/ambibottom/status") path = "/sim/status";
            if (path == "/json/info") {
                reply(fd, 200, "application/json", "{\"ver\":\"sim\",\"freeheap\":200000}");
                close(fd);
                return;
            }
            if (path.rfind("/sim/heap", 0) == 0) {  // v0.47: /sim/heap?free=N&block=M
                const std::string q = target.find('?') == std::string::npos ? std::string() : target.substr(target.find('?') + 1);
                auto num = [&](const char* k, size_t d) -> size_t {
                    const size_t i = q.find(std::string(k) + "=");
                    return i == std::string::npos ? d : static_cast<size_t>(std::atol(q.c_str() + i + std::strlen(k) + 1));
                };
                g_heap_free = num("free", g_heap_free);
                g_heap_block = num("block", g_heap_block);
                reply(fd, 200, "text/plain", "ok\n");
            } else if (path == "/sim/press") {
                app.press_link_button();
                reply(fd, 200, "text/plain", "pressed\n");
            } else if (path == "/sim/status") {
                reply(fd, 200, "application/json", app.status_json(now_s()));
            } else if (path == "/sim/leds") {
                std::lock_guard<std::mutex> lk(g_leds_mu);
                std::ostringstream o;
                o << "{\"released\":" << (g_released ? "true" : "false") << ",\"leds\":[";
                for (size_t i = 0; i < g_leds.size(); ++i)
                    o << (i ? "," : "") << "[" << int(g_leds[i].r) << "," << int(g_leds[i].g) << "," << int(g_leds[i].b) << "]";
                o << "]}";
                reply(fd, 200, "application/json", o.str());
            } else if (path == "/sim/ssdp") {
                const size_t q = target.find("st=");
                const std::string st = q == std::string::npos ? "ssdp:all" : target.substr(q + 3);
                std::string out;
                for (const auto& r : app.ssdp_replies("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nST: " + st + "\r\n\r\n")) out += r + "----\n";
                out += "==== notify\n";
                for (const auto& r : app.ssdp_notifies()) out += r + "----\n";
                reply(fd, 200, "text/plain", out);
            } else if (path == "/sim/strip") {
                strip.led_count = static_cast<int>(json_number(body, "led_count", strip.led_count));
                strip.budget_ma = json_number(body, "budget_ma", strip.budget_ma);
                strip.ma_per_led = json_number(body, "ma_per_led", strip.ma_per_led);
                strip.ma_scale_r = json_number(body, "ma_scale_r", strip.ma_scale_r);
                strip.ma_scale_g = json_number(body, "ma_scale_g", strip.ma_scale_g);
                strip.ma_scale_b = json_number(body, "ma_scale_b", strip.ma_scale_b);
                strip.brightness = json_number(body, "brightness", strip.brightness);
                strip.rise_ms = json_number(body, "rise_ms", strip.rise_ms);
                app.set_strip(strip);
                reply(fd, 200, "text/plain", "ok\n");
            } else if (path == "/ambi/el.json") {
                std::ifstream f(g_page_path.substr(0, g_page_path.size() - 5) + "_el_patch.json");
                std::stringstream ss;
                ss << f.rdbuf();
                reply(fd, ss.str().empty() ? 500 : 200, "application/json", ss.str());
            } else if (path == "/ambi" || path == "/ambi/" || path == "/ambi/en" || path == "/ambi/el") {
                // v0.37: the page as the firmware serves it (tools/make_esp_page.py writes it);
                // v0.45: /ambi/el = the Greek page
                // v0.49: /ambi/el = the loader, /ambi/el.json = the patch (as the firmware)
                std::string fn = g_page_path;
                if (path == "/ambi/el") fn = fn.substr(0, fn.size() - 5) + "_el_loader.html";
                std::ifstream f(fn);
                std::stringstream ss;
                ss << f.rdbuf();
                if (ss.str().empty()) reply(fd, 500, "text/plain", "page not found: " + g_page_path + "\n");
                else reply(fd, 200, "text/html; charset=utf-8", ss.str());
            } else if (path.rfind("/ambi/api/", 0) == 0) {
                const std::string sub = path.substr(9);  // "/state", "/service", ...
                if (sub == "/state") {
                    reply(fd, 200, "application/json", app.page_state_json(sim_page_info(app, strip)));
                } else if (sub == "/service" && method == "POST") {
                    const bool start = body.find("\"start\"") != std::string::npos;
                    const bool stop = body.find("\"stop\"") != std::string::npos;
                    if (start == stop) {
                        reply(fd, 400, "application/json", "{\"error\":\"action must be start or stop\"}");
                    } else {
                        app.set_running(start);
                        reply(fd, 200, "application/json", std::string("{\"ok\":true,\"service\":\"") + (start ? "running" : "stopped") + "\"}");
                    }
                } else if (sub == "/pair" && method == "POST") {
                    app.press_link_button();
                    reply(fd, 200, "application/json", "{\"ok\":true}");
                } else if (sub == "/pair/forget" && method == "POST") {  // v0.40
                    app.forget_pairing();
                    app.note("every paired TV forgotten (from the page)");
                    reply(fd, 200, "application/json", "{\"ok\":true}");
                } else if (sub == "/restart" && method == "POST") {  // v0.40: the sim only notes it
                    app.note("restart from the page (simulator: not restarted)");
                    reply(fd, 200, "application/json", "{\"ok\":true}");
                } else if (sub == "/diag" && method == "GET") {  // v0.44: the usermod's side simulated, the App's real
                    reply(fd, 200, "application/json",
                          "{\"app\":" + app.diag_json() +
                          ",\"version\":\"sim\",\"uptime_s\":600,\"reset_reason\":\"power_on\",\"boots\":3,\"bad_resets\":0,"
                          "\"last_bad_reset\":\"\",\"watchdog_s\":10,\"rssi\":-58,\"wifi_drops\":0,\"heap_free\":61000,"
                          "\"heap_min\":42000,\"heap_block\":38000,\"stack_dtls\":4100,\"stack_js\":3900,\"stack_loop\":2800,"
                          "\"stack_web\":3000,\"fs_used\":24576,\"fs_total\":4063232,\"app_size\":1753965,\"app_slot\":2097152,"
                          "\"output_age_ms\":40,\"wled_length\":43,\"leds_used\":43,\"budget_ma\":" + std::to_string((int)strip.budget_ma) +
                          ",\"ma_per_led\":" + std::to_string((int)strip.ma_per_led) + ",\"wled_ma_max\":" + std::to_string((int)strip.budget_ma) +
                          ",\"lamps\":3,\"pin\":38,\"time_ok\":true,\"ntp\":false,\"ntp_age_s\":-1,\"udp_realtime\":false,"
                          "\"udp_sync_recv\":false,\"cfg_needs_pin\":true}");
                } else if (sub.rfind("/capture", 0) == 0 && method == "GET") {  // v0.44 (?from=N)
                    const size_t q = target.find("from=");
                    reply(fd, 200, "application/json", app.capture_json(q == std::string::npos ? 0 : std::strtoul(target.c_str() + q + 5, nullptr, 10)));
                } else if (sub == "/capture" && method == "POST") {
                    if (body.find("\"on\":true") != std::string::npos) app.capture(true);
                    else if (body.find("\"on\":false") != std::string::npos) app.capture(false);
                    reply(fd, 200, "application/json", app.capture_json(0x7fffffff));
                } else if (sub == "/security" && method == "GET") {  // v0.42: the usermod's answer, simulated
                    reply(fd, 200, "application/json", sim_security_json(app));
                } else if (sub == "/security" && method == "POST") {
                    const bool on = body.find("\"hue_only_tv\":true") != std::string::npos;
                    const bool off = body.find("\"hue_only_tv\":false") != std::string::npos;
                    if (on == off) reply(fd, 400, "application/json", "{\"error\":\"hue_only_tv_bool\"}");
                    else if (on && app.tv_ip_seen().empty()) reply(fd, 409, "application/json", "{\"error\":\"tv_not_seen\"}");
                    else {
                        g_sim_dev.hue_only_ip = on ? app.tv_ip_seen() : "";
                        reply(fd, 200, "application/json", sim_security_json(app));
                    }
                } else if (sub == "/strip_stats" && method == "GET") {  // v0.41
                    reply(fd, 200, "application/json", app.strip_stats_json(true));
                } else if (sub == "/strip_stats/reset" && method == "POST") {
                    app.strip_stats_reset();
                    reply(fd, 200, "application/json", app.strip_stats_json(true));
                } else if (sub == "/strip_stats/install" && method == "POST") {
                    app.strip_stats_install();
                    reply(fd, 200, "application/json", app.strip_stats_json(true));
                } else if (sub == "/stats_save" && method == "POST") {  // sim only: what the usermod's loop does
                    reply(fd, 200, "application/json", app.save_stats_if_due() ? "{\"saved\":true}" : "{\"saved\":false}");
                } else if (sub == "/settings/device" && method == "GET") {  // v0.40: the usermod's answer, simulated
                    static const auto t0 = std::chrono::steady_clock::now();
                    const long up = (long)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
                    reply(fd, 200, "application/json",
                          "{\"available\":true,\"can_edit\":true,\"chip\":\"Linux-sim\",\"wled_length\":" + std::to_string(g_sim_dev.length) +
                          ",\"max_length\":1000,\"pin\":" + std::to_string(g_sim_dev.pin) +
                          ",\"pins\":[1,2,5,6,7,8,38,39],\"lamps\":" + std::to_string(g_sim_dev.lamps) +
                          ",\"button\":\"" + g_sim_dev.button + "\",\"button_pin\":41,\"name\":\"" + g_sim_dev.name + "\"" +
                          ",\"auto_start\":" + (g_sim_dev.auto_start ? "true" : "false") +
                          ",\"wled_ma_max\":" + std::to_string((int)strip.budget_ma) + ",\"wled_ma_per_led\":" +
                          std::to_string((int)strip.ma_per_led) + ",\"uptime_s\":" + std::to_string(up + 600) +
                          ",\"demo_enabled\":" + (g_sim_dev.demo_enabled ? "true" : "false") +
                          ",\"night_on\":" + (app.night().on ? "true" : "false") + ",\"night_from\":" + std::to_string(app.night().from_h) +
                          ",\"esp_ma\":" + std::to_string((int)strip.esp_ma) + ",\"night_to\":" + std::to_string(app.night().to_h) + ",\"night_pct\":" + std::to_string(app.night().pct) + ",\"tz\":" + std::to_string(g_sim_dev.tz) + ",\"ntp\":" + (g_sim_dev.ntp ? "true" : "false") +
                          ",\"time_ok\":true,\"time\":\"2026-11-19 18:00\"}");
                } else if (sub == "/settings/device" && method == "POST") {
                    const double len = json_number(body, "wled_length", g_sim_dev.length);
                    const double pin = json_number(body, "pin", g_sim_dev.pin);
                    const double lamps = json_number(body, "lamps", g_sim_dev.lamps);
                    if (len < 1 || len > 1000) reply(fd, 422, "application/json", "{\"error\":\"wled_length_out_of_range\"}");
                    else if (pin != 1 && pin != 2 && (pin < 5 || pin > 8) && pin != 38 && pin != 39)
                        reply(fd, 422, "application/json", "{\"error\":\"pin_not_allowed\"}");
                    else if (lamps != 3 && lamps != 5) reply(fd, 422, "application/json", "{\"error\":\"lamps_3_or_5\"}");
                    else if (json_number(body, "tz", 3) < 0 || json_number(body, "tz", 3) > 24)
                        reply(fd, 422, "application/json", "{\"error\":\"tz_out_of_range\"}");
                    else {
                        const bool restart = (int)pin != g_sim_dev.pin || (int)lamps != g_sim_dev.lamps;
                        g_sim_dev.length = (int)len;
                        g_sim_dev.pin = (int)pin;
                        g_sim_dev.lamps = (int)lamps;
                        if (body.find("\"button\":\"ambi\"") != std::string::npos) g_sim_dev.button = "ambi";
                        if (body.find("\"button\":\"wled\"") != std::string::npos) g_sim_dev.button = "wled";
                        g_sim_dev.tz = (int)json_number(body, "tz", g_sim_dev.tz);
                        {  // v0.44: night limit straight into the App (as the usermod does)
                            ambi_bottom::Night n = app.night();
                            if (body.find("\"night_on\":true") != std::string::npos) n.on = true;
                            if (body.find("\"night_on\":false") != std::string::npos) n.on = false;
                            n.from_h = (int)json_number(body, "night_from", n.from_h);
                            n.to_h = (int)json_number(body, "night_to", n.to_h);
                            n.pct = (int)json_number(body, "night_pct", n.pct);
                            app.set_night(n);
                        }
                        {  // v0.49: the board's share of the strip's port, as the usermod passes it
                            double e = json_number(body, "esp_ma", -1);
                            if (e > 400) e = -1;  // the usermod answers 422; the simulator just ignores it
                            if (e >= 0) { strip.esp_ma = e; app.set_strip(strip); }
                        }
                        if (body.find("\"demo_enabled\":true") != std::string::npos) g_sim_dev.demo_enabled = true;
                        if (body.find("\"demo_enabled\":false") != std::string::npos) { g_sim_dev.demo_enabled = false; app.post_demo("{\"on\":false}"); }
                        if (body.find("\"ntp\":true") != std::string::npos) g_sim_dev.ntp = true;
                        if (body.find("\"ntp\":false") != std::string::npos) g_sim_dev.ntp = false;
                        if (body.find("\"auto_start\":true") != std::string::npos) g_sim_dev.auto_start = true;
                        if (body.find("\"auto_start\":false") != std::string::npos) g_sim_dev.auto_start = false;
                        reply(fd, 200, "application/json", restart ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true,\"restart\":false}");
                    }
                } else if (sub == "/login" && method == "GET") {
                    reply(fd, 200, "application/json", "{\"available\":false,\"local\":false,\"configured\":false,\"logged_in\":false,\"can_edit\":true}");
                } else if (sub == "/settings/strip" && method == "GET") {
                    reply(fd, 200, "application/json", app.strip_settings_json(true, kSimWledLength));
                } else if (sub == "/settings/strip" && method == "POST") {
                    std::string out;
                    ambi_bottom::StripConfig applied;
                    const int code = app.post_strip_settings(body, kSimWledLength, &out, &applied);
                    if (code == 200) strip = applied;
                    reply(fd, code, "application/json", out);
                } else if (sub == "/settings/wb_slots" && method == "GET") {  // v0.50
                    reply(fd, 200, "application/json", app.wb_slots_json(true));
                } else if (sub == "/settings/wb_slots" && method == "POST") {
                    std::string out;
                    ambi_bottom::StripConfig applied;
                    bool changed = false;
                    const int code = app.post_wb_slot(body, kSimWledLength, &out, &applied, &changed);
                    if (code == 200 && changed) strip = applied;
                    reply(fd, code, "application/json", out);
                } else if (sub == "/settings/jointspace" && method == "GET") {
                    reply(fd, 200, "application/json", app.jointspace_settings_json(true));
                } else if (sub == "/settings/jointspace" && method == "POST") {
                    std::string out;
                    const int code = app.post_jointspace_settings(body, &out);
                    reply(fd, code, "application/json", out);
                } else if (sub == "/jointspace/pair" && method == "GET") {  // v0.45
                    reply(fd, 200, "application/json", app.js_pair_json());
                } else if (sub == "/jointspace/pair" && method == "POST") {
                    std::string out;
                    const int code = app.js_pair_start(body, &out);
                    reply(fd, code, "application/json", out);
                } else if (sub == "/jointspace/pair/pin" && method == "POST") {
                    std::string out;
                    const int code = app.js_pair_pin(body, &out);
                    reply(fd, code, "application/json", out);
                } else if (sub == "/log" && method == "GET") {
                    reply(fd, 200, "application/json", app.log_json());
                } else if (sub == "/demo" && method == "GET") {
                    reply(fd, 200, "application/json", app.demo_json());
                } else if (sub == "/demo" && method == "POST" && !g_sim_dev.demo_enabled &&
                           body.find("\"on\":true") != std::string::npos) {  // v0.43, as the usermod
                    reply(fd, 409, "application/json", "{\"error\":\"demo_disabled\"}");
                } else if (sub == "/demo" && method == "POST") {
                    if (app.post_demo(body)) reply(fd, 200, "application/json", app.demo_json());
                    else reply(fd, 400, "application/json", "{\"error\":\"invalid_json\"}");
                } else {
                    reply(fd, 404, "application/json", "{\"error\":\"not_on_esp\"}");
                }
            } else if (path == "/json/eff") {
                reply(fd, 200, "application/json", kSimEffects);
            } else if (path == "/json/fxdata") {
                reply(fd, 200, "application/json", kSimFxData);
            } else if (ambi_bottom::App::owns_path(path)) {
                const auto r = app.http(method, target, body, "127.0.0.1", "sim");
                reply(fd, r.status, r.content_type, r.body);
            } else {
                reply(fd, 404, "text/plain", "not the bridge's path (on the ESP32 this is WLED's own page)\n");
            }
            close(fd);
        }).detach();
    }
}
