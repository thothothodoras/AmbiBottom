// Everything the AmbiBottom usermod does that is not WLED: the fake Hue
// bridge (REST via shared/ambi_core/hue_core, pairing with a link button,
// SSDP texts), the DTLS stream server, the colour pipeline and the "TV locked
// to LightGroup" state. No Arduino, WLED or ESP-IDF header is used here, so
// the Linux test suite runs this same code (tests/test_esp_app.cpp); the
// usermod header only connects it to AsyncWebServer, AsyncUDP, NVS, the
// button and the LEDs.
//
// This header is kept free of C++17-only constructs: it is included from
// WLED's own translation units.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ambi_bottom {

// What the platform has to provide.
struct Platform {
    // Small persistent strings (ESP32: NVS). load: "" when the key is not there.
    std::function<std::string(const char* key)> load;
    std::function<bool(const char* key, const std::string& value)> save;
    // Cryptographic random bytes (ESP32: esp_fill_random).
    std::function<void(uint8_t* out, size_t len)> random;
    // v0.41: the LED wear log (strip_stats) as one text file (ESP32: WLED's filesystem).
    // Called from the task that calls save_stats_if_due() / the page's reset. Missing = in memory only.
    std::function<std::string()> stats_load;
    std::function<bool(const std::string& text)> stats_save;
    // v0.41: local wall clock in seconds since 1970 (WLED: localTime), 0 = not known yet.
    std::function<double()> local_epoch_s;
    // v0.47: free heap now and the largest free block (bytes). The JointSpace task opens a TLS
    // connection (~45 KB on the ESP32) only with enough of both, and drops it when the heap runs
    // low (v0.46 on the device: heap_min 656 B and panics while DTLS + JointSpace + the page ran).
    // Missing = no guard (Linux).
    std::function<void(size_t* free_now, size_t* largest_block)> heap;
};

struct Config {
    std::string ip;                  // our address on the LAN
    uint16_t http_port = 80;         // the TV fetches http://<ip>/description.xml: must be 80 on the real thing
    uint16_t stream_port = 2100;     // Hue Entertainment
    // Bridge MAC as the TV sees it ("00:17:88:xx:xx:xx"). Not measured whether
    // the TV insists on the Philips prefix 00:17:88; the usermod's default
    // keeps it and takes the last three bytes from the ESP's own MAC.
    std::string mac = "00:17:88:a4:5e:01";
    std::string name = "AmbiBottom";
    int lamps = 3;                   // 3 or 5 virtual lamps
    std::string tv_ip;               // when set, only this address may pair (plus anyone while the button window is open)
    double link_window_s = 30.0;     // how long a button press allows pairing
    double tv_dtls_limit_s = 3.0;    // the TV's handshake limit (from its decompiled code, not measured)
    double dtls_idle_timeout_s = 5.0;
};

// The strip settings the usermod exposes (the rest keep the pipeline's
// defaults). Validated with the same settings::sanitize() as the Linux bridge.
struct StripConfig {
    int led_count = 43;
    double ma_per_led = 40.0;        // full white; v0.40: 39 measured 2026-11-19 (v0.34: 45, v0.33: 60 = spec)
    double budget_ma = 450.0;        // hard limit for the LEDs (never above max_budget_ma())
    double esp_ma = 0.0;             // v0.49: the board's own current from the same port (0 = own port)
    // v0.33: each channel's draw relative to a third of white; 1 / 1 / 1 = as before. Not measured.
    double ma_scale_r = 1.0, ma_scale_g = 1.0, ma_scale_b = 1.0;
    double brightness = 1.0;         // 0..1
    double rise_ms = 120.0;
    double peak_emphasis = 0.5;
    std::string layout = "tv";       // "tv" | "inset" | "custom"
    std::vector<double> lamp_positions;  // for "custom": 0..1 per lamp, negative = not shown
};

struct Rgb {
    uint8_t r, g, b;
};

// v0.33: the build-time ceiling of budget_ma (AMBI_MAX_BUDGET_MA, default 900 mA).
double max_budget_ma();
// v0.49.2: what the limiter really gives the strip: budget_ma (at most max_budget_ma()) minus the
// board's share when it shares the port (color_pipeline::effective_budget_ma, one formula for both).
double strip_budget_ma(double budget_ma, double esp_ma);

struct Status {
    int paired_users = 0;
    long link_remaining_ms = 0;      // > 0: pairing is open
    bool tv_wants_pair = false;      // a pairing request was refused in the last 30 s
    bool stream_active = false;      // the TV set stream.active
    bool in_session = false;         // a DTLS session is open
    bool live = false;               // frames are arriving
    double fps = 0.0;
    unsigned long frames = 0;
    int lamps_in_stream = 0;
    std::string phase;               // idle | live | hold | fade | black | stopped
    double current_ma = 0.0;
    bool tv_locked = false;          // "the TV locked itself to LightGroup after a DTLS failure: restart the TV"
    std::string tv_lock_reason;      // timeout | failed
    std::string tv_lock_detail;
    double tv_lock_age_s = 0.0;
    bool tv_lock_confirmed = false;
    double handshake_latency_s = -1.0;
    bool old_session = false;
};

// One colour for a status LED, by priority:
// red = TV locked, magenta = old session, blue (blinking by the caller) =
// pairing open, yellow = TV wants to pair, green = streaming, dim white = idle.
enum class Indicator { idle, streaming, pairing_open, tv_wants_pair, old_session, tv_locked, not_paired };

// v0.37: the page's Demo (a WLED effect on the strip while the TV is not
// streaming). Only the request; the usermod applies it with WLED's own effects.
struct Demo {
    bool on = false;
    int fx = -1;           // WLED effect id; -1 = pick a calm one at random
    int bri_pct = 25;      // 1..100 of WLED's brightness (WLED's ABL still applies)
    int speed = 60;        // WLED effect speed 0..255
};

// v0.37: what the usermod knows that the App does not, for the page's state.
// v0.44: «Νυχτερινό όριο» - between from_h and to_h (local time, wraps over midnight) the TV's
// colours are shown at pct % of the normal brightness. Needs the clock (no clock = not applied).
struct Night {
    bool on = false;
    int from_h = 23, to_h = 7;
    int pct = 40;
};

struct PageInfo {
    std::string version;          // e.g. "v0.37-alpha (ESP32-S3)"
    std::string output_target;    // shown as the "WLED" target, e.g. "G38, 43 LED"
    int wled_led_count = 0;       // WLED's Length = all LEDs on the strip (the page's led_count = the ones used)
    bool demo_showing = false;    // the strip shows the demo effect right now
    int demo_fx = -1;             // the effect it shows
    std::string demo_fx_name;
    std::vector<Rgb> demo_leds;   // WLED's pixels as shown (after its brightness)
    double demo_ma = 0.0;         // WLED's own current estimate for them
    double output_fps = 0.0;      // frames given to WLED per second
    bool can_edit = true;         // WLED settings PIN entered (or none set)
    long free_heap = -1;
    unsigned long loop_count = 0;  // usermod loop() calls since boot (is WLED's loop running?)
    // v0.38: memory for the page's "Τεχνικά δεδομένα" (-1 = unknown)
    long heap_total = -1, heap_min = -1, heap_max_block = -1;
    long app_size = -1, app_slot = -1, flash_size = -1, fs_used = -1, fs_total = -1;
};

class App {
public:
    App(Platform platform, Config config);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ---- HTTP (any thread) --------------------------------------------------
    struct HttpResponse {
        int status;
        std::string content_type, body;
    };
    // True for the paths the fake bridge owns: /description.xml and /api...
    static bool owns_path(const std::string& path);
    HttpResponse http(const std::string& method, const std::string& path, const std::string& body,
                      const std::string& src_ip, const std::string& user_agent);

    // ---- SSDP (texts only; the caller owns the UDP socket) -------------------
    // Replies to send back to the sender of `datagram` (empty: not for us).
    std::vector<std::string> ssdp_replies(const std::string& datagram);
    // The NOTIFY set, to multicast every ~30 s.
    std::vector<std::string> ssdp_notifies();

    // ---- pairing ---------------------------------------------------------------
    void press_link_button();
    void forget_pairing();  // removes every paired user (the TV has to pair again)

    // ---- stream -------------------------------------------------------------------
    // Blocking: the DTLS server loop. Run it on its own task; stop() ends it.
    void run_dtls();
    void stop();

    // ---- output, from the LED loop (every 20-40 ms) ----------------------------------
    void set_strip(const StripConfig& strip);
    // True when `leds` was filled with new values to show. *release = true once
    // when the stream has been gone long enough to hand the strip back.
    bool poll_leds(double now_s, std::vector<Rgb>& leds, bool* release);

    Status status(double now_s);
    Indicator indicator(double now_s);
    std::string status_json(double now_s);
    std::string bridge_id();  // v0.38: "001788FFFEA1B2C3" (for the mDNS name)

    // ---- v0.37: the page (/ambi) -------------------------------------------------
    // Start / Stop: stopped = the bridge still answers the TV (pairing, REST,
    // DTLS - a refused handshake would lock the TV until it is restarted) but
    // the strip is left to WLED. The usermod boots stopped.
    void set_running(bool running);
    bool running();
    // v0.42: while a firmware update is received: the JointSpace HTTPS connection is closed
    // (≈45 KB of heap; with the TV streaming, OTA uploads broke at 0.2-0.85 MB on 2026-11-19).
    void pause_jointspace(bool paused);
    // v0.42: the TV's address as seen on its Hue stream ("" = not seen since boot)
    std::string tv_ip_seen();
    // ---- v0.44: diagnostics, Hue capture, night limit -----------------------------------------
    // The App's counters for the page's «Διάγνωση» (read-only: nothing is started or reset):
    // handshakes ok / failed (+ streak, last error), frames, fps, last frame age, bad frames,
    // stream / session / old-session / TV-lock state, paired TVs, JointSpace status / last good
    // reply age / failures, running, demo, night active.
    std::string diag_json();
    bool js_connected() const;  // v0.48: a JointSpace TLS connection is open (for the black box)
    // «Καταγραφή Hue»: while on, every frame the TV sends is kept in a small ring (128 rows):
    // the raw 16-bit lamp values as received (before brightness / limit / rise), the colour space,
    // the first / middle / last LED sent, the current, and JointSpace's two bottom corners.
    // The page fetches the new rows every second (capture_json(from) = rows with seq > from) and
    // builds the CSV itself, so the ESP keeps only ~7 KB. Stops by itself after 10 minutes.
    void capture(bool on);
    std::string capture_json(unsigned long from);
    void set_night(const Night& n);
    Night night();
    void set_demo(const Demo& demo);
    Demo demo();
    // The Linux monitor's /api/state shape ("ambilight-monitor/1"), so the
    // same page script draws it.
    std::string page_state_json(const PageInfo& info);
    // The Linux monitor's GET /api/settings/strip shape.
    std::string strip_settings_json(bool can_edit, int wled_led_count);
    // POST /api/settings/strip: partial merge, clamped (settings::merge_json),
    // led_count (the LEDs used) at most WLED's Length, saved under "strip". Returns the HTTP
    // status; *reply = the body; *applied = the settings now in force.
    int post_strip_settings(const std::string& body, int wled_led_count, std::string* reply,
                            StripConfig* applied);
    // ---- v0.50: three named white-balance slots («Θέσεις μνήμης»), kept on the board ------
    // NVS keys wb_slot1..3, each {"name","wb":[r,g,b],"keep_pure"}. GET shape:
    // {"can_edit","slots":[{"n":1,"name","wb","keep_pure"} | {"n":2,"empty":true}, ...]}.
    std::string wb_slots_json(bool can_edit);
    // POST {"slot":1..3,"action":"save"|"load"|"clear","name":"..."}. save: the white balance in
    // force now (name: 1-24 characters, none of < > & " ' \ or control; empty = keep the slot's
    // name, else "Slot N"). load: applied like POST /api/settings/strip with wb_r/g/b +
    // wb_keep_pure (*applied, *strip_changed = true). Reply: the GET shape. 422 slot_1_3 /
    // name_1_24_plain / slot_empty / action_save_load_clear, 400 invalid_json.
    int post_wb_slot(const std::string& body, int wled_led_count, std::string* reply,
                     StripConfig* applied, bool* strip_changed);
    // ---- v0.37: JointSpace (the TV's own left / top / right zones), read-only ----------
    // Blocking: polls https://<TV>:1926/6/ambilight/processed every ~300 ms on one
    // kept-alive connection (shared/ambi_core/jointspace_fetch), only while the TV
    // streams Hue (the connection is closed otherwise, which frees its ~45 KB). The
    // TV's address: the one saved from the page, else the TV's DTLS address. Run it
    // on its own task; stop() ends it.
    void run_jointspace();
    // The Linux monitor's GET /api/settings/jointspace shape (never the values).
    std::string jointspace_settings_json(bool can_edit);
    // POST {"username":..,"password":..[,"tv_ip":..]}: saved in NVS (js_user / js_pass / js_ip).
    int post_jointspace_settings(const std::string& body, std::string* reply);
    // v0.45: JointSpace pairing from the page (the TV shows a PIN). Runs on run_jointspace()'s task
    // (it has the stack for TLS); these only ask for it. start: the TV's address = tv_ip if given,
    // else the saved one, else the TV's Hue address. 409 when no address is known, 409 for a PIN
    // with no pairing waiting. On success the credentials are saved exactly as by
    // post_jointspace_settings. js_pair_json: {"state": idle | requesting | waiting_pin | granting |
    // done | failed, "error", "tv_ip", "timeout_s", "age_s"} (never the credentials).
    // body: {"tv_ip":"..."} (optional, "" = automatic) / {"pin":"1234"}
    int js_pair_start(const std::string& body, std::string* reply);
    int js_pair_pin(const std::string& body, std::string* reply);
    std::string js_pair_json();
    // ---- v0.37: a small log of what the TV (or anyone) did, for /ambi/api/log ----------
    // The last 60 events: REST requests (user names shortened), DTLS handshakes and old
    // sessions, SSDP searches answered. No secrets (no client keys, no JointSpace values).
    void note(const std::string& text);
    std::string log_json();
    // POST /api/demo body {"on":..,"fx":..,"bri_pct":..,"speed":..} (missing = unchanged).
    // Returns false for a body that is not a JSON object.
    bool post_demo(const std::string& body);
    std::string demo_json();
    // ---- v0.41: the LED wear log («Καταπόνηση LED»), as on the Linux bridge ------------------
    // GET /api/strip_stats shape (+ can_edit).
    std::string strip_stats_json(bool can_edit);
    void strip_stats_reset();    // clears "since reset"
    void strip_stats_install();  // «the strip was mounted / replaced now»: since_install starts
    // Saves the log when it is due (every few minutes while it changes). Call from the task
    // that owns the filesystem (the usermod: WLED's loop).
    bool save_stats_if_due();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace ambi_bottom
