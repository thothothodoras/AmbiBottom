// AmbiBottom - a WLED usermod (WLED 0.15.x, ESP32-S3) that makes the ESP32
// look like a Philips Hue bridge to a Philips Ambilight TV, receives the
// TV's "bottom" colours over Hue Entertainment (DTLS) and shows them on the
// LED strip WLED drives.
//
// This file is only the WLED side: web server routes, the SSDP socket, NVS,
// the button, the status LED, the pixels and the settings page. Everything
// else - the Hue REST logic, pairing, the DTLS server, the colour pipeline -
// is in the two libraries it links (lib/AmbiBottomEsp and the repository's
// shared/ambi_core), which the Linux bridge and its tests use too.
//
// Port 80: the TV fetches http://<ip>/description.xml and talks to
// http://<ip>/api/..., always on port 80, so the fake bridge cannot have a
// server of its own next to WLED's. It registers its two paths on WLED's
// AsyncWebServer instead. WLED's Alexa emulation (Espalexa) answers the very
// same paths (it also pretends to be a Hue bridge), so the build must have
// -D WLED_DISABLE_ALEXA; the #error below enforces it.
//
// NOT tested on hardware (2026-11-12): it compiles and links for
// esp32s3dev_8MB, and the logic behind it is tested on Linux. See README.md
// next to this file for what that leaves open.
#pragma once

#include "wled.h"

#ifndef ARDUINO_ARCH_ESP32
#error "AmbiBottom needs an ESP32 (DTLS and the colour pipeline do not fit an ESP8266)"
#endif
#ifndef WLED_DISABLE_ALEXA
#error "AmbiBottom and WLED's Alexa emulation both answer /description.xml and /api: build with -D WLED_DISABLE_ALEXA"
#endif

#include <AsyncUDP.h>
#include <mdns.h>
#include <esp_random.h>
#include <esp_system.h>  // v0.44: esp_reset_reason()
#include <nvs.h>
#include <nvs_flash.h>

#include <esp_debug_helpers.h>  // v0.48: backtrace of a failed allocation
#include <esp_heap_caps.h>
#include "ambi_bottom_app.h"
#include "ambi_page_gz.h"  // v0.37: the page (/ambi), made by tools/make_esp_page.py

#include <mutex>

// ---- v0.48: "black box" (user: restarts when entering the PIN; v0.47 on the device: heap_min 148 B,
// bad_resets 5 -> 17). A record in RTC memory that survives a panic / watchdog reset: the last failed
// heap allocation (size, free heap then, 8 return addresses - symbolized off the device with the
// version's firmware.elf), the lowest free heap seen by loop() and what was going on then.
// Shown in «Διάγνωση» after the next bad reset.
struct AmbiBlackBox {
  uint32_t magic;
  uint32_t fails;            // failed allocations since boot
  uint32_t fail_size, fail_free, fail_up_s;
  uint32_t fail_pc[8];
  uint32_t min_free, min_up_s;
  char min_api[24];          // the page request served last when the minimum was seen
  uint8_t min_streaming, min_js;
  char last_api[24];
  uint32_t up_s;
};
static constexpr uint32_t kBlackBoxMagic = 0xA3B0B048;
static RTC_NOINIT_ATTR AmbiBlackBox g_box;

static void IRAM_ATTR ambiAllocFailed(size_t size, uint32_t caps, const char* fn) {
  (void)caps;
  (void)fn;
  g_box.fails++;
  g_box.fail_size = size;
  g_box.fail_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_box.fail_up_s = (uint32_t)(esp_timer_get_time() / 1000000);
  esp_backtrace_frame_t fr;
  esp_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc);
  for (int i = 0; i < 8; i++) {
    g_box.fail_pc[i] = esp_cpu_process_stack_pc(fr.pc);
    if (!esp_backtrace_get_next_frame(&fr)) {
      for (int k = i + 1; k < 8; k++) g_box.fail_pc[k] = 0;
      break;
    }
  }
}

#ifndef AMBIBOTTOM_VERSION
#define AMBIBOTTOM_VERSION "v0.50-beta"
#endif
#ifndef USERMOD_ID_AMBIBOTTOM
#define USERMOD_ID_AMBIBOTTOM 0xAB  // not an id WLED has given out; only used for the pin owner tag
#endif
// AtomS3 Lite: button G41, on-board RGB LED G35, Grove/strip data G5 (WLED's own LED pin setting).
#ifndef AMBIBOTTOM_LINK_PIN
#define AMBIBOTTOM_LINK_PIN 41
#endif
#ifndef AMBIBOTTOM_STATUS_PIN
#define AMBIBOTTOM_STATUS_PIN 35
#endif
// v0.40: the pins the page offers for the strip's data (AtomS3 Lite: G5-G8, G38, G39 on the
// header, G1 / G2 on the Grove port; G41 = the button, G35 = the on-board RGB LED)
#ifndef AMBIBOTTOM_DATA_PINS
#define AMBIBOTTOM_DATA_PINS 1, 2, 5, 6, 7, 8, 38, 39
#endif
#ifndef AMBIBOTTOM_MAX_LENGTH
#define AMBIBOTTOM_MAX_LENGTH 1000  // the page's limit for WLED's Length (all LEDs on the strip)
#endif

class AmbiBottomUsermod : public Usermod {
private:
  // ---- settings (WLED's Usermods page, saved in cfg.json) -------------------
  bool enabled = true;
  int8_t linkPin = AMBIBOTTOM_LINK_PIN;      // pairing ("link") button, active low
  static constexpr uint32_t kPageHeapFloor = 24000;  // v0.48: page API answers 503 below this
  int8_t statusPin = AMBIBOTTOM_STATUS_PIN;  // one WS2812 for the state, -1 = none

  uint8_t lamps = 3;                         // 3 or 5 virtual lamps
  String tvIp = "";                          // optional: only this address may pair
  String bridgeMac = "";                     // "" = 00:17:88 + the last 3 bytes of the ESP's MAC
  uint16_t ledsUsed = 0;                     // v0.37: LEDs used from the start of the strip (0 = all of WLED's Length); the rest are kept dark
  uint16_t budgetMa = 450;                   // current limit for the strip (never above the build's ceiling, 900 mA)
  uint16_t espSamePortMa = 0;                // v0.49: the board's own current when it shares the strip's USB port (0 = own port)
  uint8_t maPerLed = 40;                     // full white, per LED (v0.40: 39 mA measured 2026-11-19, + a little)
  // v0.33: each colour channel's draw in % of a third of white (100 / 100 / 100 = as before; not measured)
  uint16_t maScaleRPct = 100, maScaleGPct = 100, maScaleBPct = 100;
  uint8_t brightnessPct = 100;
  uint16_t riseMs = 120;
  uint8_t peakEmphasisPct = 50;
  String layout = "tv";                      // tv | inset | custom
  String lampPositions = "";                 // custom layout: "0,0.5,1" (one per lamp, -1 = not shown)
  uint8_t dtlsLimitS = 3;                    // the TV's handshake limit, see tv_lock.hpp
  bool autoStart = false;                    // v0.40: Start by itself after boot (the page's choice)
  bool demoEnabled = false;
  // v0.44: «Νυχτερινό όριο» (see ambi_bottom::Night)
  bool nightOn = false;
  uint8_t nightFrom = 23, nightTo = 7, nightPct = 40;                  // v0.43 (user): the Demo exists only with this option on (Συσκευή)
  String hueOnlyIp = "";                     // v0.42: "" = anyone on the LAN may talk Hue (pairing still needs the button); else only this address

  // ---- state --------------------------------------------------------------------
  ambi_bottom::App* app = nullptr;
  nvs_handle_t nvs = 0;  // namespace "ambibottom": pairing + groups; survives reboot, OTA and a WLED config reset
  bool nvsOpen = false;
  AsyncUDP ssdp;
  bool ssdpListening = false;
  TaskHandle_t dtlsTask = nullptr;
  TaskHandle_t jsTask = nullptr;
  uint32_t lastSsdpIp = 0;
  unsigned long lastSsdpLog = 0;
  bool initDone = false;
  bool driving = false;
  bool savedGammaOff = false;
  unsigned long lastPoll = 0, lastNotify = 0, lastStatusLed = 0, buttonDownAt = 0;
  bool buttonWasDown = false;
  std::vector<ambi_bottom::Rgb> leds;

  // ---- v0.37: Start / Stop / Demo from the page (/ambi) ------------------------
  // Boots stopped: the strip is WLED's (effects, presets, the app). Start: the
  // TV's colours, black while the TV does not stream, or the Demo effect.
  bool wasRunning = false;
  bool savedForceMaxBri = false;
  uint8_t savedBri = 0, savedBriLast = 128, savedMode = 0, savedSpeed = 128, savedIntensity = 128, savedPalette = 0;
  bool demoApplied = false;              // WLED shows the demo effect (we put it there)
  bool demoTouched = false;              // WLED's effect / brightness were changed since Start: Stop restores them
  ambi_bottom::Demo appliedDemo;
  int demoFx = -1;                       // the effect actually shown
  unsigned long lastBlack = 0, lastPageInfo = 0, fpsWindowStart = 0;
  unsigned fpsFrames = 0;
  volatile unsigned long loopCount = 0;
  unsigned lastLength = 0;
  // v0.38: memory figures for the page (the image size is read once: getSketchSize() reads the whole image)
  long appSize = -1, appSlot = -1, flashSize = -1, fsUsed = -1, fsTotal = -1;
  unsigned long lastFsCheck = 0;               // WLED's Length the pipeline was given (follows changes at once)  // v0.37 diagnostics: is WLED's loop calling us?
  float outputFps = 0;
  std::mutex pageMu;                     // the async web task reads pageInfo
  ambi_bottom::PageInfo pageInfo;
  std::mutex pendingMu;                  // strip settings saved from the page, applied in loop()
  bool pendingStrip = false;
  ambi_bottom::StripConfig pendingCfg;
  // v0.40: the page's "ESP & WLED" settings (-1 = unchanged), applied in loop()
  struct PendingDevice {
    bool any = false, restart = false;
    int length = -1, pin = -1, lamps = -1, autoStart = -1;
    int button = -1;          // v0.41: 1 = the Atom's button opens pairing (ours), 0 = WLED's button 0
    int tz = -1, ntp = -1;    // v0.41: WLED's time zone (0..24) and internet time (NTP) on / off
    int demo = -1;            // v0.43: the Demo option
    int nightOn = -1, nightFrom = -1, nightTo = -1, nightPct = -1;  // v0.44
    int espMa = -1;  // v0.49
    String name;              // v0.41: WLED's device name = the bridge's name to the TV ("" = unchanged)
  } pendingDev;
  // v0.40: one place for the current: the page's limit / mA per LED <-> WLED's Maximum current / LED current
  bool hwSynced = false;
  bool hwPushPending = false;  // v0.49.2: readFromConfig runs inside WLED's JSON lock: push from loop()
  int lastWledAbl = -1, lastWledMa = -1;
  unsigned long lastHwSync = 0, hwPushAt = 0;
  unsigned long lastStatsSave = 0;  // v0.41
  // v0.44: diagnostics
  unsigned long lastOutMs = 0;      // the last frame of the TV given to the strip
  unsigned wifiDrops = 0;
  bool wifiWasUp = false;
  uint32_t bootCount = 0, badResets = 0;
  String lastBadReset;
  AmbiBlackBox lastBox{};   // v0.48: the black box of the last bad reset
  bool lastBoxValid = false;
  // v0.42: Stop while the TV is on the strip -> fade our last frame out, then hand the strip to WLED
  static constexpr unsigned long kStopFadeMs = 800;
  unsigned long stopFadeStart = 0;
  std::vector<ambi_bottom::Rgb> stopFrom;

  static const char _name[];

  static double nowS() { return millis() / 1000.0; }

  String defaultMac() {
    uint8_t m[6];
    WiFi.macAddress(m);
    char buf[18];
    snprintf(buf, sizeof(buf), "00:17:88:%02x:%02x:%02x", m[3], m[4], m[5]);
    return String(buf);
  }

  // v0.37: the LEDs this usermod uses (and lets WLED light); the ones after it are kept dark
  unsigned usedLeds() const {
    const unsigned len = strip.getLengthTotal();
    return (ledsUsed > 0 && ledsUsed < len) ? ledsUsed : len;
  }

  ambi_bottom::StripConfig stripConfig() {
    ambi_bottom::StripConfig c;
    c.led_count = usedLeds();
    c.ma_per_led = maPerLed;
    c.budget_ma = budgetMa;
    c.esp_ma = espSamePortMa;  // v0.49
    c.ma_scale_r = maScaleRPct / 100.0;
    c.ma_scale_g = maScaleGPct / 100.0;
    c.ma_scale_b = maScaleBPct / 100.0;
    c.brightness = brightnessPct / 100.0;
    c.rise_ms = riseMs;
    c.peak_emphasis = peakEmphasisPct / 100.0;
    c.layout = layout.c_str();
    c.lamp_positions.clear();
    int start = 0;
    while (start < (int)lampPositions.length()) {
      int comma = lampPositions.indexOf(',', start);
      if (comma < 0) comma = lampPositions.length();
      String part = lampPositions.substring(start, comma);
      part.trim();
      if (part.length()) c.lamp_positions.push_back(part.toFloat());
      start = comma + 1;
    }
    return c;
  }

  void startApp() {
    if (app || !enabled) return;
    if (!nvsOpen) nvsOpen = nvs_open("ambibottom", NVS_READWRITE, &nvs) == ESP_OK;
    ambi_bottom::Platform p;
    p.load = [this](const char* key) {
      std::string out;
      size_t len = 0;
      if (!nvsOpen || nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) return out;
      out.resize(len);  // len counts the terminating zero
      if (nvs_get_str(nvs, key, &out[0], &len) != ESP_OK) return std::string();
      out.resize(len - 1);
      return out;
    };
    p.save = [this](const char* key, const std::string& value) {
      // one NVS string holds up to 4000 bytes; pairing and groups are a few hundred
      return nvsOpen && nvs_set_str(nvs, key, value.c_str()) == ESP_OK && nvs_commit(nvs) == ESP_OK;
    };
    p.random = [](uint8_t* out, size_t len) { esp_fill_random(out, len); };
    // v0.41: the LED wear log in WLED's filesystem (read here, written from loop() only)
    p.stats_load = []() -> std::string {
      File f = WLED_FS.open(F("/ambi_stats.json"), "r");
      if (!f) return std::string();
      std::string out;
      const size_t size = f.size();
      if (size > 0 && size < 64 * 1024) {
        out.resize(size);
        if (f.read(reinterpret_cast<uint8_t*>(&out[0]), size) != size) out.clear();
      }
      f.close();
      return out;
    };
    p.stats_save = [](const std::string& text) {
      File f = WLED_FS.open(F("/ambi_stats.tmp"), "w");
      if (!f) return false;
      const bool ok = f.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) == text.size();
      f.close();
      if (!ok) return false;
      WLED_FS.remove(F("/ambi_stats.json"));
      return WLED_FS.rename(F("/ambi_stats.tmp"), F("/ambi_stats.json"));
    };
    // v0.41: WLED's local time (its time zone), 0 until WLED knows the time (NTP, or a browser that opened its page)
    p.local_epoch_s = []() -> double { return toki.getTimeSource() > TOKI_TS_NONE ? (double)localTime : 0.0; };
    p.heap = [](size_t* f, size_t* b) {  // v0.47: the JointSpace memory guard
      *f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      *b = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    };
    ambi_bottom::Config c;
    c.ip = Network.localIP().toString().c_str();
    c.http_port = 80;
    c.mac = (bridgeMac.length() == 17 ? bridgeMac : defaultMac()).c_str();
    c.name = serverDescription;  // WLED's own device name
    c.lamps = lamps == 5 ? 5 : 3;
    c.tv_ip = tvIp.c_str();
    c.tv_dtls_limit_s = dtlsLimitS;
    app = new ambi_bottom::App(p, c);
    app->set_strip(stripConfig());
    applyNight();  // v0.44
    if (autoStart) {  // v0.40: the page's "Αυτόματο Start"
      app->set_running(true);
      app->note("auto Start after boot");
    }
    // mbedTLS handshake + the pipeline need a real stack; 12 KB measured nowhere yet (see README).
    // v0.37: pinned to core 0 (WLED's loop runs on core 1) at priority 3: on 2026-11-18 WLED's
    // loop stopped being run on the device while this task (then priority 4, either core) was up.
    xTaskCreatePinnedToCore([](void* self) {
      static_cast<AmbiBottomUsermod*>(self)->app->run_dtls();
      vTaskDelete(nullptr);
    }, "ambi_dtls", 12288, this, 3, &dtlsTask, 0);
    // v0.37: JointSpace (the TV's sides), HTTPS client: mbedTLS handshake on this stack. Core 0,
    // below the DTLS task; it only talks to the TV while the TV streams Hue.
    xTaskCreatePinnedToCore([](void* self) {
      static_cast<AmbiBottomUsermod*>(self)->app->run_jointspace();
      vTaskDelete(nullptr);
    }, "ambi_js", 12288, this, 2, &jsTask, 0);
    if (ssdp.listenMulticast(IPAddress(239, 255, 255, 250), 1900)) {
      ssdpListening = true;
      ssdp.onPacket([this](AsyncUDPPacket packet) {
        if (!app) return;
        std::string msg(reinterpret_cast<const char*>(packet.data()), packet.length());
        const std::vector<std::string> replies = app->ssdp_replies(msg);
        for (const std::string& reply : replies) {
          packet.write(reinterpret_cast<const uint8_t*>(reply.data()), reply.size());
        }
        // v0.37 log: one line per asker per 10 s
        if (!replies.empty()) {
          const uint32_t ip = (uint32_t)packet.remoteIP();
          const unsigned long now = millis();
          if (ip != lastSsdpIp || now - lastSsdpLog > 10000) {
            lastSsdpIp = ip;
            lastSsdpLog = now;
            app->note(std::string("SSDP search from ") + packet.remoteIP().toString().c_str() + " answered");
          }
        }
      });
    }
    announceHue();
  }

  // v0.38: _hue._tcp named like a real bridge. The TV's mDNS search (AmbiHueNsdReceiver) only
  // takes a service whose NAME contains "Hue" and ends in the bridge's last 6 hex digits (its
  // MAC); WLED's own instance name failed that, so the TV fell back to UPnP (5 s) and the
  // internet check, about 10 s longer every time it looked for the bridge (2026-11-18 log).
  std::string hueName, hueId;
  void announceHue() {
    if (!app) return;
    const std::string id = app->bridge_id();  // "001788FFFEA1B2C3"
    hueName = "Philips Hue - " + id.substr(id.size() >= 6 ? id.size() - 6 : 0);
    hueId = id;
    for (char& c : hueId) c = (char)tolower((unsigned char)c);
    mdns_txt_item_t txt[2] = {{"bridgeid", hueId.c_str()}, {"modelid", "BSB002"}};
    if (mdns_service_add(hueName.c_str(), "_hue", "_tcp", 80, txt, 2) != ESP_OK) {
      // already there (WLED re-connected): set the name and records again
      mdns_service_instance_name_set("_hue", "_tcp", hueName.c_str());
      mdns_service_txt_set("_hue", "_tcp", txt, 2);
    }
  }

  // The request body. WLED 16's AsyncWebServer does NOT pass a body to the body
  // callback when the Content-Type is application/x-www-form-urlencoded (curl -d,
  // and the default of Java's HttpURLConnection): it parses it as form fields
  // instead, a JSON text starting with { or [ becoming one field named "body"
  // (split at '&', URL-decoded). Seen on the device 2026-11-18: pairing without
  // a clientkey, groups created empty. So: the raw body when there is one, else
  // the form fields put back together.
  static String takeBody(AsyncWebServerRequest* request) {
    String body;
    if (request->_tempObject) {
      body = static_cast<std::string*>(request->_tempObject)->c_str();
      delete static_cast<std::string*>(request->_tempObject);
      request->_tempObject = nullptr;
      return body;
    }
    const size_t n = request->params();
    for (size_t i = 0; i < n; i++) {
      const AsyncWebParameter* prm = request->getParam(i);
      if (!prm || !prm->isPost() || prm->isFile()) continue;
      if (body.length()) body += '&';
      if (prm->name() != F("body")) {
        body += prm->name();
        body += '=';
      }
      body += prm->value();
    }
    return body;
  }

  void handleHttp(AsyncWebServerRequest* request) {
    if (!app) {
      request->send(503, "application/json", "[{\"error\":{\"type\":901,\"description\":\"bridge not started (no network yet)\"}}]");
      return;
    }
    const std::string body = takeBody(request).c_str();
    // v0.42 (security, the page's «Hue μόνο από την TV»): /api answers only the TV's address.
    // description.xml stays open (it holds nothing secret and the TV reads it while it searches).
    if (hueOnlyIp.length() && request->url().startsWith(F("/api")) &&
        request->client()->remoteIP().toString() != hueOnlyIp) {
      request->send(403, "application/json", F("[{\"error\":{\"type\":1,\"description\":\"unauthorized: only the TV may use this bridge\"}}]"));
      return;
    }
    const char* method = request->method() == HTTP_GET ? "GET" : request->method() == HTTP_PUT ? "PUT" :
                         request->method() == HTTP_POST ? "POST" : request->method() == HTTP_DELETE ? "DELETE" : "GET";
    String ua = request->hasHeader("User-Agent") ? request->header("User-Agent") : String("");
    const ambi_bottom::App::HttpResponse r =
        app->http(method, request->url().c_str(), body, request->client()->remoteIP().toString().c_str(), ua.c_str());
    request->send(r.status, r.content_type.c_str(), r.body.c_str());
  }

  static void collectBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (total > 4096) return;  // nothing the TV sends is near this
    if (index == 0) request->_tempObject = new std::string();
    if (request->_tempObject) static_cast<std::string*>(request->_tempObject)->append(reinterpret_cast<char*>(data), len);
  }

  void showIndicator() {
    if (statusPin < 0 || !app) return;
    const bool blink = (millis() / 300) % 2;
    uint8_t r = 0, g = 0, b = 0;
    switch (app->indicator(nowS())) {
      case ambi_bottom::Indicator::tv_locked:     r = 60; break;                       // red: restart the TV
      case ambi_bottom::Indicator::old_session:   r = 40; b = 40; break;               // magenta
      case ambi_bottom::Indicator::pairing_open:  b = blink ? 60 : 0; break;           // blinking blue
      case ambi_bottom::Indicator::tv_wants_pair: r = 50; g = 30; break;               // yellow: press the button
      case ambi_bottom::Indicator::streaming:     g = 25; break;                       // green
      case ambi_bottom::Indicator::not_paired:    r = blink ? 20 : 0; g = blink ? 12 : 0; break;
      default:                                    r = g = b = 3; break;                // idle: dim white
    }
    neopixelWrite(statusPin, r, g, b);
  }

  // v0.49.2: the black box's 8 return addresses as "xxxxxxxx xxxxxxxx ..." (alloc_last and last_crash)
  static constexpr size_t kPcsLen = 8 * 9 + 1;
  static void formatPcs(const uint32_t* pc, char (&out)[kPcsLen]) {
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < 8 && pc[i] && n < kPcsLen - 1; i++)
      n += snprintf(out + n, kPcsLen - n, "%s%08lx", i ? " " : "", (unsigned long)pc[i]);
  }

  // ---- v0.40: WLED's LED settings from the page ------------------------------------------
  static int wledPin() {
    Bus* b = BusManager::getBus(0);
    uint8_t pins[5] = {255, 255, 255, 255, 255};
    if (!b || b->getPins(pins) == 0) return -1;
    return pins[0];
  }
  static bool dataPinAllowed(int pin) {
    static const int allowed[] = {AMBIBOTTOM_DATA_PINS};
    for (int a : allowed)
      if (a == pin) return true;
    return false;
  }

  // Changes WLED's first LED output the way POST /json/cfg does (WLED's whole "hw" object with
  // a few values changed: a partial one would reset the rest), -1 = unchanged. WLED re-creates
  // the output in its own loop and saves cfg.json; reboot: after that (a new pin).
  bool applyWledHw(int length, int pin, int ledma, int maxpwr, bool reboot, int btnPin = -2) {
    if (!requestJSONBufferLock(JSON_LOCK_CFG_DES)) return false;
    JsonObject root = pDoc->to<JsonObject>();
    serializeConfig(root);
    std::vector<String> other;
    for (JsonPair kv : root)
      if (strcmp(kv.key().c_str(), "hw") != 0) other.push_back(kv.key().c_str());
    for (const String& k : other) root.remove(k);
    JsonObject led = root["hw"]["led"];
    JsonObject bus = led["ins"][0];
    bool ok = !bus.isNull();
    if (ok) {
      if (maxpwr >= 0) led["maxpwr"] = maxpwr;
      if (length > 0) { bus["len"] = length; led["total"] = length; }
      if (pin >= 0) bus["pin"][0] = pin;
      if (ledma > 0) bus["ledma"] = ledma;
      // v0.44: without a change to the output itself, WLED must not re-create it (the strip went
      // dark for a moment when only the button was switched, 2026-11-19). Only "ins" goes: without
      // the rest of hw.led WLED would reset e.g. its target FPS to the default.
      if (length <= 0 && pin < 0 && ledma <= 0) led.remove("ins");
      if (btnPin != -2) {  // v0.41: WLED's button 0 (-1 = none); WLED frees / takes the pin itself
        JsonArray ins = root["hw"]["btn"]["ins"];
        if (ins.isNull()) ins = root["hw"]["btn"].createNestedArray("ins");
        JsonObject b0 = ins.size() ? ins[0].as<JsonObject>() : ins.createNestedObject();
        if (b0["type"].isNull()) b0["type"] = BTN_TYPE_PUSH;
        JsonArray bp = b0["pin"];
        if (bp.isNull()) bp = b0.createNestedArray("pin");
        if (bp.size()) bp[0] = btnPin;
        else bp.add(btnPin);
      }
      if (reboot) root["rb"] = true;
      deserializeConfig(root);
      configNeedsWrite = true;
    }
    releaseJSONBufferLock();
    return ok;
  }

  // v0.49.2: what WLED's Maximum current should be: the strip's share (the page's limit minus the
  // board's share of the port, the limiter's own formula, so a limit below 50 mA stays as it is)
  // + MA_FOR_ESP, which WLED itself takes off that value for the board (bus_manager.cpp). Our
  // builds set -D MA_FOR_ESP=0 (README: the board has its own port), so there it is the strip's
  // share; a build without the flag (120 mA on an ESP32) still gives the strip what the page says.
  // Never below MA_FOR_ESP + 1: 0 = WLED's limiter OFF, and WLED 0.15 also skips it below MA_FOR_ESP.
  int stripShareMa() const { return (int)(ambi_bottom::strip_budget_ma(budgetMa, espSamePortMa) + 0.5); }
  int wledAblTarget() const {
    const int t = stripShareMa() + MA_FOR_ESP;
    return t > MA_FOR_ESP ? t : MA_FOR_ESP + 1;
  }
  // WLED's value -> the page's (port) limit: the strip's share WLED gives + the board's share
  int portFromWledAbl(int abl) const {
    const int cap = (int)ambi_bottom::max_budget_ma();
    const int strip = abl > MA_FOR_ESP ? abl - MA_FOR_ESP : 0;
    return min(strip + (int)espSamePortMa, cap);
  }

  // the page's limit and mA per LED -> WLED, when they differ (WLED's ABL off = left off)
  void pushCurrentToWled() {
    Bus* b = BusManager::getBus(0);
    if (!b) return;
    const int abl = BusManager::ablMilliampsMax(), ma = b->getLEDCurrent();
    const int target = wledAblTarget();  // v0.49.2: the strip's share + WLED's own MA_FOR_ESP
    const int wantAbl = abl > 0 && abl != target ? target : -1;
    const int wantMa = ma > 0 && ma != maPerLed ? maPerLed : -1;
    if (wantAbl < 0 && wantMa < 0) return;
    if (applyWledHw(-1, -1, wantMa, wantAbl, false)) {
      hwPushAt = millis();
      app->note(std::string("WLED Maximum current / LED current set from the page: ") + std::to_string(target) + " mA (strip " +
                std::to_string(stripShareMa()) + " + board " + std::to_string(MA_FOR_ESP) + "), " + std::to_string(maPerLed) +
                " mA per LED");
    }
  }

  // every second: WLED's LED Preferences changed by hand -> the page takes the new values
  void syncCurrentWithWled(unsigned long now) {
    if (now - lastHwSync < 1000) return;
    lastHwSync = now;
    if (hwPushAt && now - hwPushAt < 3000) return;  // WLED is still re-creating the output
    Bus* b = BusManager::getBus(0);
    if (!b) return;
    const int abl = BusManager::ablMilliampsMax(), ma = b->getLEDCurrent();
    if (!hwSynced || hwPushAt) {  // first look after boot / after our own push: the page's values win
      const bool first = !hwSynced;
      hwSynced = true;
      hwPushAt = 0;
      lastWledAbl = abl;
      lastWledMa = ma;
      if (first) pushCurrentToWled();
      return;
    }
    bool changed = false, pushBack = false;
    if (abl != lastWledAbl) {
      lastWledAbl = abl;
      // v0.49.2: WLED holds the strip's share + MA_FOR_ESP; the page's limit is the port's
      if (abl > 0 && abl != wledAblTarget()) {
        // at or below MA_FOR_ESP (only in a build without -D MA_FOR_ESP=0) WLED gives the strip
        // nothing (0.15: no limit at all): not a limit to adopt - the page's limit stays and WLED gets the right value back
        const int port = abl > MA_FOR_ESP ? portFromWledAbl(abl) : (int)budgetMa;
        if (port != budgetMa) { budgetMa = (uint16_t)port; changed = true; }
        // cut at the ceiling or below WLED's own floor: WLED gets the value that really applies
        // (it is the only limit when the TV is not streaming)
        pushBack = abl != wledAblTarget();
      }
    }
    if (ma != lastWledMa) {
      lastWledMa = ma;
      if (ma > 0 && ma <= 255 && ma != maPerLed) { maPerLed = (uint8_t)ma; changed = true; }
    }
    if (changed) {
      app->set_strip(stripConfig());
      configNeedsWrite = true;
      app->note(std::string("WLED LED Preferences changed: the page now uses ") + std::to_string(budgetMa) + " mA, " +
                std::to_string(maPerLed) + " mA per LED");
    }
    if (pushBack) pushCurrentToWled();
  }

  // v0.41: who has the Atom's button (G41): "ambi" (pairing), "wled" (WLED's button 0) or "none"
  String buttonOwner() const {
    if (linkPin >= 0) return F("ambi");
    for (const auto& b : buttons)
      if (b.pin == AMBIBOTTOM_LINK_PIN) return F("wled");
    return F("none");
  }
  static bool nameOk(const String& n) {
    if (n.length() < 1 || n.length() > 32) return false;
    for (size_t i = 0; i < n.length(); i++) {
      const char c = n[i];
      if (c < 32 || c > 126 || c == '<' || c == '>' || c == '&' || c == '"' || c == '\'') return false;
    }
    return true;
  }

  // v0.44: why the ESP started last time
  static const char* resetReasonText(esp_reset_reason_t r) {
    switch (r) {
      case ESP_RST_POWERON: return "power_on";
      case ESP_RST_EXT: return "external";
      case ESP_RST_SW: return "software";
      case ESP_RST_PANIC: return "panic";
      case ESP_RST_INT_WDT: return "interrupt_watchdog";
      case ESP_RST_TASK_WDT: return "task_watchdog";
      case ESP_RST_WDT: return "watchdog";
      case ESP_RST_DEEPSLEEP: return "deep_sleep";
      case ESP_RST_BROWNOUT: return "brownout";
      case ESP_RST_SDIO: return "sdio";
      default: return "unknown";
    }
  }
  static bool badReset(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
  }
  static long stackLeft(TaskHandle_t t) { return t ? (long)uxTaskGetStackHighWaterMark(t) : -1; }

  // v0.44: the ESP's side of «Διάγνωση»; the page joins it with /security and /settings/device
  String diagJson() {
    String j = F("{\"app\":");
    j += app->diag_json().c_str();
    j += F(",\"version\":\"" AMBIBOTTOM_VERSION "\"");
    j += F(",\"uptime_s\":"); j += millis() / 1000;
    j += F(",\"reset_reason\":\""); j += resetReasonText(esp_reset_reason()); j += '"';
    j += F(",\"boots\":"); j += bootCount;
    j += F(",\"bad_resets\":"); j += badResets;
    j += F(",\"last_bad_reset\":\""); j += lastBadReset; j += '"';
    j += F(",\"watchdog_s\":"); j += WLED_WATCHDOG_TIMEOUT;
    j += F(",\"rssi\":"); j += WiFi.isConnected() ? WiFi.RSSI() : 0;
    j += F(",\"wifi_drops\":"); j += wifiDrops;
    j += F(",\"heap_free\":"); j += ESP.getFreeHeap();
    j += F(",\"heap_min\":"); j += ESP.getMinFreeHeap();
    j += F(",\"heap_block\":"); j += ESP.getMaxAllocHeap();
    j += F(",\"stack_dtls\":"); j += stackLeft(dtlsTask);
    j += F(",\"stack_js\":"); j += stackLeft(jsTask);
    j += F(",\"stack_loop\":"); j += stackLeft(xTaskGetHandle("loopTask"));
    j += F(",\"stack_web\":"); j += stackLeft(xTaskGetHandle("async_tcp"));
    j += F(",\"fs_used\":"); j += fsUsed;
    j += F(",\"fs_total\":"); j += fsTotal;
    j += F(",\"app_size\":"); j += appSize;
    j += F(",\"app_slot\":"); j += appSlot;
    j += F(",\"output_age_ms\":"); j += lastOutMs ? (long)(millis() - lastOutMs) : -1L;
    j += F(",\"alloc_fails\":"); j += g_box.fails;  // v0.48: since this boot
    j += F(",\"heap_min_api\":\""); j += g_box.min_api; j += '"';
    j += F(",\"heap_min_up_s\":"); j += g_box.min_up_s;
    if (g_box.fails) {  // v0.49: this boot's last failed allocation, at once (not only after a crash)
      char pcs[kPcsLen];
      formatPcs(g_box.fail_pc, pcs);
      j += F(",\"alloc_last\":{\"size\":"); j += g_box.fail_size;
      j += F(",\"free\":"); j += g_box.fail_free;
      j += F(",\"up_s\":"); j += g_box.fail_up_s;
      j += F(",\"pc\":\""); j += pcs; j += F("\"}");
    }
    if (lastBoxValid) {  // v0.48: what the black box saw before the last bad reset
      char pcs[kPcsLen];
      formatPcs(lastBox.fail_pc, pcs);
      lastBox.min_api[sizeof(lastBox.min_api) - 1] = 0;
      lastBox.last_api[sizeof(lastBox.last_api) - 1] = 0;
      j += F(",\"last_crash\":{\"up_s\":"); j += lastBox.up_s;
      j += F(",\"alloc_fails\":"); j += lastBox.fails;
      j += F(",\"fail_size\":"); j += lastBox.fail_size;
      j += F(",\"fail_free\":"); j += lastBox.fail_free;
      j += F(",\"fail_up_s\":"); j += lastBox.fail_up_s;
      j += F(",\"fail_pc\":\""); j += pcs; j += '"';
      j += F(",\"min_free\":"); j += lastBox.min_free;
      j += F(",\"min_up_s\":"); j += lastBox.min_up_s;
      j += F(",\"min_api\":\""); j += lastBox.min_api; j += '"';
      j += F(",\"min_streaming\":"); j += lastBox.min_streaming ? F("true") : F("false");
      j += F(",\"min_js\":"); j += lastBox.min_js ? F("true") : F("false");
      j += F(",\"last_api\":\""); j += lastBox.last_api; j += F("\"}");
    }
    j += F(",\"wled_length\":"); j += strip.getLengthTotal();
    j += F(",\"leds_used\":"); j += usedLeds();
    j += F(",\"budget_ma\":"); j += budgetMa;
    j += F(",\"ma_per_led\":"); j += maPerLed;
    j += F(",\"wled_ma_max\":"); j += BusManager::ablMilliampsMax();
    j += F(",\"wled_ma_target\":"); j += wledAblTarget();  // v0.49.2: what WLED should hold (strip + MA_FOR_ESP)
    j += F(",\"strip_ma\":"); j += stripShareMa();
    j += F(",\"esp_ma\":"); j += espSamePortMa;
    j += F(",\"ma_for_esp\":"); j += MA_FOR_ESP;
    j += F(",\"lamps\":"); j += lamps;
    j += F(",\"pin\":"); j += wledPin();
    j += F(",\"time_ok\":"); j += toki.getTimeSource() > TOKI_TS_NONE ? F("true") : F("false");
    j += F(",\"ntp\":"); j += ntpEnabled ? F("true") : F("false");
    j += F(",\"ntp_age_s\":"); j += (ntpEnabled && ntpLastSyncTime != NTP_NEVER) ? (long)((millis() - ntpLastSyncTime) / 1000) : -1L;
    j += F(",\"udp_realtime\":"); j += receiveDirect ? F("true") : F("false");
    j += F(",\"udp_sync_recv\":"); j += (receiveGroups && (receiveNotificationBrightness || receiveNotificationColor || receiveNotificationEffects)) ? F("true") : F("false");
    j += F(",\"cfg_needs_pin\":true");  // v0.44: our /json/cfg guard (CVE-2026-71264) is in this build
    j += '}';
    return j;
  }

  // v0.44 (CVE-2026-71264): WLED 16.0.1 serves GET /json/cfg without the settings PIN. Our handler
  // is registered before WLED's own (usermods start before WLED's server), so it answers first.
  static void guardedCfg(AsyncWebServerRequest* request) {
    if (strlen(settingsPIN) > 0 && !correctPIN) {
      request->send(401, "application/json", F("{\"error\":{\"code\":1,\"message\":\"settings PIN required\"}}"));
      return;
    }
    serveJson(request);
  }

  // v0.42: the security card (no secret values, only whether they are set)
  String securityJson() {
    String j = F("{\"can_edit\":");
    j += correctPIN ? F("true") : F("false");
    j += F(",\"pin\":"); j += strlen(settingsPIN) > 0 ? F("true") : F("false");
    j += F(",\"ota_lock\":"); j += otaLock ? F("true") : F("false");
    j += F(",\"ota_same_subnet\":"); j += otaSameSubnet ? F("true") : F("false");
    j += F(",\"arduino_ota\":"); j += aOtaEnabled ? F("true") : F("false");
    j += F(",\"ap_default_pass\":"); j += strcmp(apPass, DEFAULT_AP_PASS) == 0 ? F("true") : F("false");
    j += F(",\"ap_behavior\":"); j += apBehavior;
    j += F(",\"hue_only_ip\":\""); j += hueOnlyIp; j += '"';
    j += F(",\"tv_ip_seen\":\""); j += app ? app->tv_ip_seen().c_str() : ""; j += '"';
    j += F(",\"js_saved\":"); j += app && app->jointspace_settings_json(true).find("\"configured\":true") != std::string::npos ? F("true") : F("false");
    j += '}';
    return j;
  }

  void applyNight() {  // v0.44
    if (!app) return;
    ambi_bottom::Night n;
    n.on = nightOn;
    n.from_h = nightFrom;
    n.to_h = nightTo;
    n.pct = nightPct;
    app->set_night(n);
  }

  String deviceSettingsJson() {
    String j = F("{\"available\":true,\"can_edit\":");
    j += correctPIN ? F("true") : F("false");
    j += F(",\"wled_length\":"); j += strip.getLengthTotal();
    j += F(",\"max_length\":"); j += AMBIBOTTOM_MAX_LENGTH;
    j += F(",\"page_el\":"); j += AMBIBOTTOM_PAGE_EL ? F("true") : F("false");  // v0.48
    j += F(",\"chip\":\""); j += ESP.getChipModel(); j += '"';  // v0.49: «ESP32-S3» / «ESP32» for the output label
    j += F(",\"pin\":"); j += wledPin();
    j += F(",\"pins\":[");
    {
      static const int allowed[] = {AMBIBOTTOM_DATA_PINS};
      bool first = true;
      for (int a : allowed) {
        if (!first) j += ',';
        first = false;
        j += a;
      }
    }
    j += F("],\"lamps\":"); j += lamps;
    j += F(",\"button\":\""); j += buttonOwner(); j += '"';
    j += F(",\"button_pin\":"); j += AMBIBOTTOM_LINK_PIN;
    {
      String n = serverDescription;  // checked on the way in (nameOk): no quote or backslash
      n.replace("\\", ""); n.replace("\"", "");
      j += F(",\"name\":\""); j += n; j += '"';
    }
    j += F(",\"auto_start\":"); j += autoStart ? F("true") : F("false");
    j += F(",\"demo_enabled\":"); j += demoEnabled ? F("true") : F("false");
    j += F(",\"night_on\":"); j += nightOn ? F("true") : F("false");
    j += F(",\"night_from\":"); j += nightFrom;
    j += F(",\"night_to\":"); j += nightTo;
    j += F(",\"night_pct\":"); j += nightPct;
    j += F(",\"esp_ma\":"); j += espSamePortMa;  // v0.49
    Bus* b = BusManager::getBus(0);
    j += F(",\"wled_ma_max\":"); j += BusManager::ablMilliampsMax();
    j += F(",\"wled_ma_per_led\":"); j += b ? b->getLEDCurrent() : 0;
    j += F(",\"uptime_s\":"); j += millis() / 1000;
    // v0.41: the clock (the wear log's days and episodes need it)
    j += F(",\"tz\":"); j += currentTimezone;
    j += F(",\"ntp\":"); j += ntpEnabled ? F("true") : F("false");
    const bool timeOk = toki.getTimeSource() > TOKI_TS_NONE;
    j += F(",\"time_ok\":"); j += timeOk ? F("true") : F("false");
    if (timeOk) {
      const time_t t = localTime;
      struct tm tmv;
      gmtime_r(&t, &tmv);  // localTime already carries the zone
      char b[24];
      strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tmv);
      j += F(",\"time\":\""); j += b; j += '"';
    }
    j += '}';
    return j;
  }

  // POST /ambi/api/settings/device {"wled_length":..,"pin":..,"lamps":..,"auto_start":..} (any of them)
  int postDeviceSettings(const String& body, String* reply) {
    StaticJsonDocument<256> d;  // ArduinoJson 6 in WLED 16
    if (body.length() > 200 || deserializeJson(d, body) || !d.is<JsonObject>()) { *reply = F("{\"error\":\"invalid_json\"}"); return 400; }
    PendingDevice p;
    bool restart = false;
    if (!d["wled_length"].isNull()) {
      const int v = d["wled_length"] | -1;
      if (v < 1 || v > AMBIBOTTOM_MAX_LENGTH) { *reply = F("{\"error\":\"wled_length_out_of_range\"}"); return 422; }
      p.length = v;
    }
    if (!d["pin"].isNull()) {
      const int v = d["pin"] | -1;
      if (!dataPinAllowed(v)) { *reply = F("{\"error\":\"pin_not_allowed\"}"); return 422; }
      if (v != wledPin()) {
        if (PinManager::isPinAllocated(v)) { *reply = F("{\"error\":\"pin_in_use\"}"); return 422; }
        p.pin = v;
        restart = true;
      }
    }
    if (!d["lamps"].isNull()) {
      const int v = d["lamps"] | -1;
      if (v != 3 && v != 5) { *reply = F("{\"error\":\"lamps_3_or_5\"}"); return 422; }
      if (v != lamps) { p.lamps = v; restart = true; }
    }
    if (!d["button"].isNull()) {  // v0.41
      const String v = d["button"] | "";
      if (v != F("ambi") && v != F("wled")) { *reply = F("{\"error\":\"button_ambi_or_wled\"}"); return 422; }
      if (v != buttonOwner()) p.button = v == F("ambi") ? 1 : 0;
    }
    if (!d["name"].isNull()) {  // v0.41
      String v = d["name"] | "";
      v.trim();
      if (!nameOk(v)) { *reply = F("{\"error\":\"name_1_to_32_plain\"}"); return 422; }
      if (v != serverDescription) { p.name = v; restart = true; }
    }
    if (!d["night_on"].isNull()) {  // v0.44
      if (!d["night_on"].is<bool>()) { *reply = F("{\"error\":\"night_on_bool\"}"); return 422; }
      p.nightOn = d["night_on"].as<bool>() ? 1 : 0;
    }
    for (const char* k : {"night_from", "night_to"}) {
      if (d[k].isNull()) continue;
      const int v = d[k] | -1;
      if (v < 0 || v > 23) { *reply = F("{\"error\":\"night_hour_0_23\"}"); return 422; }
      if (!strcmp(k, "night_from")) p.nightFrom = v;
      else p.nightTo = v;
    }
    if (!d["night_pct"].isNull()) {
      const int v = d["night_pct"] | -1;
      if (v < 5 || v > 100) { *reply = F("{\"error\":\"night_pct_5_100\"}"); return 422; }
      p.nightPct = v;
    }
    if (!d["esp_ma"].isNull()) {  // v0.49: the board's share of the strip's port, 0 = own port
      const int v = d["esp_ma"] | -1;
      if (v < 0 || v > 400) { *reply = F("{\"error\":\"esp_ma_0_400\"}"); return 422; }
      p.espMa = v;
    }
    if (!d["demo_enabled"].isNull()) {  // v0.43
      if (!d["demo_enabled"].is<bool>()) { *reply = F("{\"error\":\"demo_enabled_bool\"}"); return 422; }
      p.demo = d["demo_enabled"].as<bool>() ? 1 : 0;
    }
    if (!d["tz"].isNull()) {  // v0.41
      const int v = d["tz"] | -1;
      if (v < 0 || v > 24) { *reply = F("{\"error\":\"tz_out_of_range\"}"); return 422; }
      p.tz = v;
    }
    if (!d["ntp"].isNull()) {
      if (!d["ntp"].is<bool>()) { *reply = F("{\"error\":\"ntp_bool\"}"); return 422; }
      p.ntp = d["ntp"].as<bool>() ? 1 : 0;
    }
    if (!d["auto_start"].isNull()) {
      if (!d["auto_start"].is<bool>()) { *reply = F("{\"error\":\"auto_start_bool\"}"); return 422; }
      p.autoStart = d["auto_start"].as<bool>() ? 1 : 0;
    }
    p.any = true;
    {
      std::lock_guard<std::mutex> lk(pendingMu);
      pendingDev = p;
    }
    *reply = restart ? F("{\"ok\":true,\"restart\":true}") : F("{\"ok\":true,\"restart\":false}");
    return 200;
  }

  // ---- v0.37: the page's API (/ambi/api/...), on WLED's async web task -------------

  void handlePageApi(AsyncWebServerRequest* request) {
    const String body = takeBody(request);
    const String sub = request->url().substring(9);  // after "/ambi/api"
    const bool get = request->method() == HTTP_GET, post = request->method() == HTTP_POST;
    if (!app) {
      request->send(503, "application/json", F("{\"error\":\"not_started\"}"));
      return;
    }
    strncpy(g_box.last_api, sub.c_str(), sizeof(g_box.last_api) - 1);  // v0.48: black box
    // v0.48: below a heap floor the page waits (it retries) instead of pushing the ESP over
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kPageHeapFloor && sub != F("/diag")) {
      AsyncWebServerResponse* r = request->beginResponse(503, "application/json", F("{\"error\":\"busy_low_memory\"}"));
      r->addHeader(F("Retry-After"), F("1"));
      request->send(r);
      return;
    }
    // Same rule as WLED's own settings: with a settings PIN set, it must be entered first.
    if (post && !correctPIN) {
      request->send(401, "application/json", F("{\"error\":\"pin_required\"}"));
      return;
    }
    if (get && sub == F("/state")) {
      ambi_bottom::PageInfo info;
      {
        std::lock_guard<std::mutex> lk(pageMu);
        info = pageInfo;
      }
      info.can_edit = correctPIN;
      info.free_heap = ESP.getFreeHeap();
      info.loop_count = loopCount;
      info.heap_total = ESP.getHeapSize();
      info.heap_min = ESP.getMinFreeHeap();
      info.heap_max_block = ESP.getMaxAllocHeap();
      info.app_size = appSize;
      info.app_slot = appSlot;
      info.flash_size = flashSize;
      info.fs_used = fsUsed;
      info.fs_total = fsTotal;
      request->send(200, "application/json", app->page_state_json(info).c_str());
    } else if (post && sub == F("/service")) {
      const bool start = body.indexOf(F("\"start\"")) >= 0, stop = body.indexOf(F("\"stop\"")) >= 0;
      if (start == stop) {
        request->send(400, "application/json", F("{\"error\":\"action must be start or stop\"}"));
        return;
      }
      app->set_running(start);  // loop() does the rest
      request->send(200, "application/json", start ? F("{\"ok\":true,\"service\":\"running\"}") : F("{\"ok\":true,\"service\":\"stopped\"}"));
    } else if (post && sub == F("/pair")) {
      app->press_link_button();
      request->send(200, "application/json", F("{\"ok\":true}"));
    } else if (post && sub == F("/pair/forget")) {  // v0.40: the TV has to pair again
      app->forget_pairing();
      app->note("every paired TV forgotten (from the page)");
      request->send(200, "application/json", F("{\"ok\":true}"));
    } else if (post && sub == F("/restart")) {  // v0.40
      {
        std::lock_guard<std::mutex> lk(pendingMu);
        pendingDev.restart = true;
      }
      request->send(200, "application/json", F("{\"ok\":true}"));
    } else if (get && sub == F("/diag")) {  // v0.44: «Διάγνωση» (read-only)
      request->send(200, "application/json", diagJson());
    } else if (get && sub == F("/capture")) {  // v0.44: «Καταγραφή Hue» rows after ?from=
      const unsigned long from = request->hasParam("from") ? strtoul(request->getParam("from")->value().c_str(), nullptr, 10) : 0;
      request->send(200, "application/json", app->capture_json(from).c_str());
    } else if (post && sub == F("/capture")) {
      if (body.indexOf(F("\"on\":true")) >= 0) app->capture(true);
      else if (body.indexOf(F("\"on\":false")) >= 0) app->capture(false);
      else { request->send(400, "application/json", F("{\"error\":\"on_bool\"}")); return; }
      request->send(200, "application/json", app->capture_json(0x7fffffff).c_str());
    } else if (get && sub == F("/security")) {  // v0.42: what is protected and what is not
      request->send(200, "application/json", securityJson());
    } else if (post && sub == F("/security")) {
      StaticJsonDocument<128> d;
      if (body.length() > 100 || deserializeJson(d, body) || !d["hue_only_tv"].is<bool>()) {
        request->send(400, "application/json", F("{\"error\":\"hue_only_tv_bool\"}"));
        return;
      }
      if (d["hue_only_tv"].as<bool>()) {
        const String ip = app->tv_ip_seen().c_str();
        if (!ip.length()) {
          request->send(409, "application/json", F("{\"error\":\"tv_not_seen\"}"));
          return;
        }
        hueOnlyIp = ip;
        app->note(std::string("security: Hue API only from the TV (") + ip.c_str() + ")");
      } else {
        hueOnlyIp = "";
        app->note("security: Hue API open to the LAN again (pairing still needs the button)");
      }
      configNeedsWrite = true;  // the async task only sets it; WLED writes cfg.json from its loop
      request->send(200, "application/json", securityJson());
    } else if (get && sub == F("/strip_stats")) {  // v0.41: the LED wear log («Καταπόνηση LED»)
      request->send(200, "application/json", app->strip_stats_json(correctPIN).c_str());
    } else if (post && sub == F("/strip_stats/reset")) {
      app->strip_stats_reset();  // writes the file: the async task, but WLED never writes this one
      request->send(200, "application/json", app->strip_stats_json(true).c_str());
    } else if (post && sub == F("/strip_stats/install")) {
      app->strip_stats_install();
      request->send(200, "application/json", app->strip_stats_json(true).c_str());
    } else if (get && sub == F("/settings/device")) {  // v0.40: WLED's Length / pin, lamps, auto Start
      request->send(200, "application/json", deviceSettingsJson());
    } else if (post && sub == F("/settings/device")) {
      String reply;
      const int code = postDeviceSettings(body, &reply);
      request->send(code, "application/json", reply);
    } else if (get && sub == F("/login")) {
      request->send(200, "application/json", correctPIN ? F("{\"available\":false,\"local\":false,\"configured\":false,\"logged_in\":false,\"can_edit\":true}")
                                                       : F("{\"available\":false,\"local\":false,\"configured\":false,\"logged_in\":false,\"can_edit\":false}"));
    } else if (get && sub == F("/settings/strip")) {
      request->send(200, "application/json", app->strip_settings_json(correctPIN, strip.getLengthTotal()).c_str());
    } else if (post && sub == F("/settings/strip")) {
      std::string reply;
      ambi_bottom::StripConfig applied;
      const int code = app->post_strip_settings(body.c_str(), strip.getLengthTotal(), &reply, &applied);
      if (code == 200) {
        std::lock_guard<std::mutex> lk(pendingMu);
        pendingCfg = applied;
        pendingStrip = true;
      }
      request->send(code, "application/json", reply.c_str());
    } else if (get && sub == F("/settings/wb_slots")) {  // v0.50
      request->send(200, "application/json", app->wb_slots_json(correctPIN).c_str());
    } else if (post && sub == F("/settings/wb_slots")) {
      std::string reply;
      ambi_bottom::StripConfig applied;
      bool changed = false;
      const int code = app->post_wb_slot(body.c_str(), strip.getLengthTotal(), &reply, &applied, &changed);
      if (code == 200 && changed) {
        std::lock_guard<std::mutex> lk(pendingMu);
        pendingCfg = applied;
        pendingStrip = true;
      }
      request->send(code, "application/json", reply.c_str());
    } else if (get && sub == F("/settings/jointspace")) {
      request->send(200, "application/json", app->jointspace_settings_json(correctPIN).c_str());
    } else if (post && sub == F("/settings/jointspace")) {
      std::string reply;
      const int code = app->post_jointspace_settings(body.c_str(), &reply);
      request->send(code, "application/json", reply.c_str());
    } else if (get && sub == F("/jointspace/pair")) {  // v0.45: JointSpace pairing from the page
      request->send(200, "application/json", app->js_pair_json().c_str());
    } else if (post && sub == F("/jointspace/pair")) {
      std::string reply;
      const int code = app->js_pair_start(body.c_str(), &reply);
      request->send(code, "application/json", reply.c_str());
    } else if (post && sub == F("/jointspace/pair/pin")) {
      std::string reply;
      const int code = app->js_pair_pin(body.c_str(), &reply);
      request->send(code, "application/json", reply.c_str());
    } else if (get && sub == F("/log")) {
      request->send(200, "application/json", app->log_json().c_str());
    } else if (get && sub == F("/demo")) {
      request->send(200, "application/json", app->demo_json().c_str());
    } else if (post && sub == F("/demo")) {
      if (!demoEnabled && body.indexOf(F("\"on\":true")) >= 0) {  // v0.43: the option is off
        request->send(409, "application/json", F("{\"error\":\"demo_disabled\"}"));
        return;
      }
      if (app->post_demo(body.c_str())) request->send(200, "application/json", app->demo_json().c_str());
      else request->send(400, "application/json", F("{\"error\":\"invalid_json\"}"));
    } else {
      request->send(404, "application/json", F("{\"error\":\"not_on_esp\"}"));  // Linux-only parts of the page
    }
  }

  // ---- v0.37: what Start / Stop / Demo do to WLED (loop() only) ----------------------
  // A calm effect for "random": the calm ones the page marks with ☾ that colour
  // themselves from a palette (Breathe / Fade / Candle use the segment's own colour,
  // which may be anything - on the bench it was plain red).
  int randomCalmEffect() {
    // v0.37: the five simple ones the page lists (user 2026-11-18: "5 απλά εφέ")
    static const char* const calm[] = {"Rainbow", "Colorloop", "Pacifica", "Aurora", "Colorwaves"};
    int found[16];
    int n = 0;
    for (unsigned id = 1; id < strip.getModeCount() && n < 16; id++) {
      char name[24];
      strncpy_P(name, strip.getModeData(id), sizeof(name) - 1);
      name[sizeof(name) - 1] = 0;
      char* at = strchr(name, '@');
      if (at) *at = 0;
      for (const char* c : calm)
        if (strcmp(name, c) == 0) found[n++] = (int)id;
    }
    return n ? found[esp_random() % n] : 2;  // 2 = Breathe in every WLED so far
  }

  void onStart() {
    Segment& seg = strip.getMainSegment();
    savedBri = bri;
    savedBriLast = briLast;  // WLED's "brightness when switched on again"
    savedMode = seg.mode;
    savedSpeed = seg.speed;
    savedIntensity = seg.intensity;
    savedPalette = seg.palette;
    savedForceMaxBri = arlsForceMaxBri;
    arlsForceMaxBri = true;  // the TV's frames are already limited by our pipeline: WLED's brightness must not scale them
    demoApplied = false;
    demoTouched = false;
    lastBlack = 0;
  }

  void onStop() {
    if (driving) {
      driving = false;
      arlsDisableGammaCorrection = savedGammaOff;
    }
    if (realtimeMode) exitRealtime();
    arlsForceMaxBri = savedForceMaxBri;
    if (demoTouched) {  // give WLED back what it showed before Start
      Segment& seg = strip.getMainSegment();
      seg.setMode(savedMode);
      seg.speed = savedSpeed;
      seg.intensity = savedIntensity;
      seg.setPalette(savedPalette);
      bri = savedBri;
      briLast = savedBriLast;
      stateChanged = true;
      stateUpdated(CALL_MODE_DIRECT_CHANGE);
    }
    demoApplied = false;
    demoTouched = false;
  }

  void applyDemo(const ambi_bottom::Demo& d) {
    if (realtimeMode) exitRealtime();
    int fx = d.fx;
    if (fx < 0 || fx >= (int)strip.getModeCount()) {
      // keep the random pick while only brightness / speed change
      fx = (demoApplied && appliedDemo.fx < 0 && demoFx >= 0) ? demoFx : randomCalmEffect();
    }
    Segment& seg = strip.getMainSegment();
    if (!demoApplied || fx != demoFx) seg.setMode(fx, true);
    seg.speed = d.speed;
    seg.setOption(SEG_OPTION_ON, true);
    bri = (uint8_t)max(1, d.bri_pct * 255 / 100);
    stateChanged = true;
    stateUpdated(CALL_MODE_DIRECT_CHANGE);
    demoApplied = true;
    demoTouched = true;
    appliedDemo = d;
    demoFx = fx;
  }

  static bool sameDemo(const ambi_bottom::Demo& a, const ambi_bottom::Demo& b) {
    return a.on == b.on && a.fx == b.fx && a.bri_pct == b.bri_pct && a.speed == b.speed;
  }

  // the page's view of the strip: refreshed here, read by the web task
  void updatePageInfo(unsigned long now) {
    if (lastFsCheck == 0 || now - lastFsCheck >= 10000) {  // loop() task: the filesystem is WLED's
      lastFsCheck = now ? now : 1;
      fsUsed = (long)WLED_FS.usedBytes();
      fsTotal = (long)WLED_FS.totalBytes();
    }
    if (now - lastPageInfo < 100) return;
    lastPageInfo = now;
    ambi_bottom::PageInfo info;
    info.version = AMBIBOTTOM_VERSION " (ESP32-S3)";
    const unsigned n = usedLeds();
    info.wled_led_count = strip.getLengthTotal();
    info.output_target = (String(F("this ESP, ")) + n + F(" of ") + strip.getLengthTotal() + F(" LED")).c_str();
    info.output_fps = outputFps;
    info.demo_showing = demoApplied && !driving && app->running();
    info.demo_fx = demoApplied ? demoFx : -1;
    if (demoApplied && demoFx >= 0) {
      char name[24];
      strncpy_P(name, strip.getModeData(demoFx), sizeof(name) - 1);
      name[sizeof(name) - 1] = 0;
      char* at = strchr(name, '@');
      if (at) *at = 0;
      info.demo_fx_name = name;
    }
    if (info.demo_showing) {
      const unsigned b = strip.getBrightness();
      info.demo_leds.reserve(n);
      for (unsigned i = 0; i < n; i++) {
        const uint32_t c = strip.getPixelColor(i);
        info.demo_leds.push_back(ambi_bottom::Rgb{(uint8_t)(R(c) * b / 255), (uint8_t)(G(c) * b / 255), (uint8_t)(B(c) * b / 255)});
      }
      const unsigned used = BusManager::currentMilliamps();
      info.demo_ma = used > MA_FOR_ESP ? used - MA_FOR_ESP : 0;
    }
    std::lock_guard<std::mutex> lk(pageMu);
    pageInfo = std::move(info);
  }

public:
  // v0.37: called by WLED before every show() (effects AND realtime): the LEDs after the
  // ones we use are kept dark, whoever draws (user: "να ανάβουν όσα δηλώνουμε").
  void handleOverlayDraw() override {
    if (!enabled) return;
    const unsigned len = strip.getLengthTotal(), used = usedLeds();
    for (unsigned i = used; i < len; i++) strip.setPixelColor(i, (uint32_t)0);
  }

  void setup() override {
    // v0.44: boot / unexpected-reset counters (NVS, written once per boot)
    {
      nvs_handle_t h;
      if (nvs_open("ambibottom", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t v = 0;
        nvs_get_u32(h, "boots", &v);
        bootCount = v + 1;
        nvs_set_u32(h, "boots", bootCount);
        v = 0;
        nvs_get_u32(h, "bad_resets", &v);
        badResets = v;
        size_t len = 0;
        if (nvs_get_str(h, "bad_reason", nullptr, &len) == ESP_OK && len > 0 && len < 40) {
          char b[40];
          if (nvs_get_str(h, "bad_reason", b, &len) == ESP_OK) lastBadReset = b;
        }
        const esp_reset_reason_t r = esp_reset_reason();
        // v0.48: the black box survives software / panic / watchdog resets, not a power cut
        if (badReset(r) && g_box.magic == kBlackBoxMagic) {
          lastBox = g_box;
          lastBoxValid = true;
        }
        memset(&g_box, 0, sizeof(g_box));
        g_box.magic = kBlackBoxMagic;
        g_box.min_free = 0xFFFFFFFF;
        heap_caps_register_failed_alloc_callback(ambiAllocFailed);
        if (badReset(r)) {
          badResets++;
          lastBadReset = resetReasonText(r);
          nvs_set_u32(h, "bad_resets", badResets);
          nvs_set_str(h, "bad_reason", lastBadReset.c_str());
        }
        nvs_commit(h);
        nvs_close(h);
      }
    }
    appSize = (long)ESP.getSketchSize();
    appSlot = (long)ESP.getFreeSketchSpace();  // the size of the OTA slot an update goes into
    flashSize = (long)ESP.getFlashChipSize();
    if (enabled && linkPin >= 0 && PinManager::allocatePin(linkPin, false, PinOwner::UM_Unspecified)) {
      pinMode(linkPin, INPUT_PULLUP);
    } else {
      linkPin = -1;
    }
    if (statusPin >= 0 && !PinManager::allocatePin(statusPin, true, PinOwner::UM_Unspecified)) statusPin = -1;
    // v0.44: /json/cfg only with the PIN (CVE-2026-71264; see guardedCfg)
    server.on("/json/cfg", HTTP_GET, [](AsyncWebServerRequest* request) { guardedCfg(request); });
    // The fake bridge's two paths, on WLED's own server (see the header comment).
    server.on("/description.xml", HTTP_GET, [this](AsyncWebServerRequest* request) { handleHttp(request); });
    server.on("/api", HTTP_ANY, [this](AsyncWebServerRequest* request) { handleHttp(request); }, nullptr, collectBody);
    // State as JSON, and the link button for when nobody can reach the real one.
    server.on("/ambibottom/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
      request->send(200, "application/json", app ? app->status_json(nowS()).c_str() : "{\"started\":false}");
    });
    server.on("/ambibottom/pair", HTTP_POST, [this](AsyncWebServerRequest* request) {
      // Same protection as WLED's own settings: with a settings PIN set, it must be entered.
      if (!correctPIN) {
        request->send(401, "text/plain", "enter the WLED settings PIN first");
        return;
      }
      if (app) app->press_link_button();
      request->send(200, "text/plain", "pairing open for 30 s");
    });
    // v0.37: the page and its API. "/ambi/api" first: a handler also takes the paths below its own.
    server.on("/ambi/api", HTTP_ANY, [this](AsyncWebServerRequest* request) { handlePageApi(request); }, nullptr, collectBody);
    // v0.45: English by default (/ambi, /ambi/en), Greek at /ambi/el. The page's EN / ΕΛ button
    // switches and the browser remembers it (the async server drops Accept-Language).
    server.on("/ambi", HTTP_GET, [](AsyncWebServerRequest* request) {
      // v0.49: Greek = a small loader (/ambi/el) + a patch on the English page (/ambi/el.json),
      // applied in the browser (saves ~44 KB of flash against a second full page)
      const String url = request->url();
#if AMBIBOTTOM_PAGE_EL
      if (url.endsWith(F("/el.json"))) {
        AsyncWebServerResponse* p = request->beginResponse_P(200, "application/json", AMBI_PAGE_EL_PATCH_GZ, AMBI_PAGE_EL_PATCH_GZ_LEN);
        p->addHeader(F("Content-Encoding"), F("gzip"));
        p->addHeader(F("Cache-Control"), F("no-cache"));
        request->send(p);
        return;
      }
      if (url.endsWith(F("/el"))) {
        AsyncWebServerResponse* l = request->beginResponse_P(200, "text/html; charset=utf-8", AMBI_PAGE_EL_LOADER);
        l->addHeader(F("Cache-Control"), F("no-cache"));
        request->send(l);
        return;
      }
#endif
      AsyncWebServerResponse* r = request->beginResponse_P(200, "text/html; charset=utf-8", AMBI_PAGE_GZ, AMBI_PAGE_GZ_LEN);
      r->addHeader(F("Content-Encoding"), F("gzip"));
      r->addHeader(F("Cache-Control"), F("no-cache"));
      request->send(r);
    });
    initDone = true;
  }

  // v0.42: WLED calls this before a firmware update (true) and after one that failed (false)
  void onUpdateBegin(bool init) override {
    if (app) app->pause_jointspace(init);
  }

  void connected() override {
    startApp();
    announceHue();  // v0.38: also after a Wi-Fi reconnect (WLED restarts mDNS then)
  }

  void loop() override {
    loopCount++;
    if (!enabled || !initDone) return;
    const unsigned long now = millis();
    {  // v0.48: black box - the lowest heap seen here and what was going on
      const uint32_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      g_box.up_s = now / 1000;
      if (f < g_box.min_free) {
        g_box.min_free = f;
        g_box.min_up_s = now / 1000;
        memcpy(g_box.min_api, g_box.last_api, sizeof(g_box.min_api));
        g_box.min_streaming = driving ? 1 : 0;
        g_box.min_js = app && app->js_connected() ? 1 : 0;
      }
    }

    // link button: a press of 50 ms .. 3 s opens pairing; 10 s forgets the TV
    if (linkPin >= 0) {
      const bool down = digitalRead(linkPin) == LOW;
      if (down && !buttonWasDown) buttonDownAt = now;
      if (!down && buttonWasDown && app) {
        const unsigned long held = now - buttonDownAt;
        if (held >= 10000) app->forget_pairing();
        else if (held >= 50 && held < 3000) app->press_link_button();
      }
      buttonWasDown = down;
    }
    if (!app) return;

    // v0.37: settings saved from the page -> the usermod's own fields (cfg.json) too
    {
      std::lock_guard<std::mutex> lk(pendingMu);
      if (pendingStrip) {
        pendingStrip = false;
        ledsUsed = pendingCfg.led_count >= (int)strip.getLengthTotal() ? 0 : (uint16_t)pendingCfg.led_count;
        budgetMa = (uint16_t)pendingCfg.budget_ma;
        maPerLed = (uint8_t)min(255.0, pendingCfg.ma_per_led + 0.5);
        maScaleRPct = (uint16_t)(pendingCfg.ma_scale_r * 100 + 0.5);
        maScaleGPct = (uint16_t)(pendingCfg.ma_scale_g * 100 + 0.5);
        maScaleBPct = (uint16_t)(pendingCfg.ma_scale_b * 100 + 0.5);
        brightnessPct = (uint8_t)(pendingCfg.brightness * 100 + 0.5);
        riseMs = (uint16_t)pendingCfg.rise_ms;
        peakEmphasisPct = (uint8_t)(pendingCfg.peak_emphasis * 100 + 0.5);
        layout = pendingCfg.layout.c_str();
        String pos;
        for (size_t i = 0; i < pendingCfg.lamp_positions.size(); i++) {
          if (i) pos += ',';
          pos += String(pendingCfg.lamp_positions[i], 3);
        }
        lampPositions = pos;
        configNeedsWrite = true;  // WLED writes cfg.json from its loop
        if (hwSynced) pushCurrentToWled();  // v0.40: WLED's Maximum current / LED current follow
      }
    }
    // v0.40: the page's "ESP & WLED" settings, restart
    {
      PendingDevice dev;
      {
        std::lock_guard<std::mutex> lk(pendingMu);
        dev = pendingDev;
        pendingDev = PendingDevice();
      }
      if (dev.any) {
        bool reboot = false;
        if (dev.autoStart >= 0 && (dev.autoStart == 1) != autoStart) { autoStart = dev.autoStart == 1; configNeedsWrite = true; }
        if (dev.lamps > 0 && dev.lamps != lamps) { lamps = (uint8_t)dev.lamps; configNeedsWrite = true; reboot = true; }
        const int len = dev.length > 0 && dev.length != (int)strip.getLengthTotal() ? dev.length : -1;
        const int pin = dev.pin >= 0 && dev.pin != wledPin() ? dev.pin : -1;
        if (pin >= 0) reboot = true;
        if (len > 0 || pin >= 0) {
          applyWledHw(len, pin, -1, -1, reboot);
          app->note(std::string("WLED LED output set from the page: ") + (len > 0 ? std::to_string(len) + " LEDs " : std::string()) +
                    (pin >= 0 ? "pin " + std::to_string(pin) : std::string()));
        }
        if (dev.button == 1 && linkPin < 0) {  // v0.41: the Atom's button -> pairing (WLED's button 0 lets go of it)
          applyWledHw(-1, -1, -1, -1, false, -1);
          if (PinManager::allocatePin(AMBIBOTTOM_LINK_PIN, false, PinOwner::UM_Unspecified)) {
            linkPin = AMBIBOTTOM_LINK_PIN;
            pinMode(linkPin, INPUT_PULLUP);
            buttonWasDown = false;
            app->note("the Atom's button opens pairing now (WLED's button 0 off)");
          } else {
            app->note("the Atom's button: pin still in use, not taken");
          }
          configNeedsWrite = true;
        } else if (dev.button == 0 && linkPin >= 0) {  // -> WLED's button 0 (on / off, presets)
          PinManager::deallocatePin(linkPin, PinOwner::UM_Unspecified);
          linkPin = -1;
          applyWledHw(-1, -1, -1, -1, false, AMBIBOTTOM_LINK_PIN);
          app->note("the Atom's button is WLED's again (pairing only from the page)");
          configNeedsWrite = true;
        }
        if (dev.nightOn >= 0 || dev.nightFrom >= 0 || dev.nightTo >= 0 || dev.nightPct >= 0) {  // v0.44
          if (dev.nightOn >= 0) nightOn = dev.nightOn == 1;
          if (dev.nightFrom >= 0) nightFrom = (uint8_t)dev.nightFrom;
          if (dev.nightTo >= 0) nightTo = (uint8_t)dev.nightTo;
          if (dev.nightPct >= 0) nightPct = (uint8_t)dev.nightPct;
          applyNight();
          configNeedsWrite = true;
        }
        if (dev.espMa >= 0 && dev.espMa != espSamePortMa) {  // v0.49
          espSamePortMa = (uint16_t)dev.espMa;
          app->set_strip(stripConfig());
          pushCurrentToWled();
          app->note(std::string("power: the board ") + (espSamePortMa ? "shares the strip's port (" + std::to_string(espSamePortMa) + " mA counted)" : "has its own port"));
          configNeedsWrite = true;
        }
        if (dev.demo >= 0 && (dev.demo == 1) != demoEnabled) {  // v0.43
          demoEnabled = dev.demo == 1;
          if (!demoEnabled) app->post_demo("{\"on\":false}");
          configNeedsWrite = true;
          app->note(demoEnabled ? "Demo option on" : "Demo option off (and the Demo stopped)");
        }
        if (dev.tz >= 0 && dev.tz != currentTimezone) {  // WLED picks the new zone up in updateLocalTime()
          currentTimezone = (byte)dev.tz;
          configNeedsWrite = true;
        }
        if (dev.ntp >= 0 && (dev.ntp == 1) != ntpEnabled) {
          ntpEnabled = dev.ntp == 1;
          if (ntpEnabled && WLED_CONNECTED && !ntpConnected) ntpConnected = ntpUdp.begin(ntpLocalPort);
          configNeedsWrite = true;
          app->note(ntpEnabled ? "internet time (NTP) on" : "internet time (NTP) off");
        }
        if (dev.name.length() && dev.name != serverDescription) {  // v0.41: the name the TV shows (description.xml)
          strlcpy(serverDescription, dev.name.c_str(), sizeof(serverDescription));
          configNeedsWrite = true;
          reboot = true;
        }
        if (reboot) { doReboot = true; app->note("restarting (pin / lamps / name changed from the page)"); }
      }
      if (dev.restart) { doReboot = true; app->note("restart from the page"); }
    }

    if (now - lastPoll >= 20) {
      lastPoll = now;
      // v0.37: WLED's Length changed (LED Preferences, /json/cfg): the pipeline follows now, not at the next boot
      if (strip.getLengthTotal() != lastLength) {
        lastLength = strip.getLengthTotal();
        app->set_strip(stripConfig());
      }
      if (hwPushPending) {
        hwPushPending = false;
        pushCurrentToWled();
      }
      syncCurrentWithWled(now);
      const bool running = app->running();
      if (running && stopFadeStart) stopFadeStart = 0;  // Start again during the fade
      if (running != wasRunning) {
        if (!running && driving && kStopFadeMs > 0) {
          // v0.42 (user: «σβήνει απότομα»): fade out what the TV showed, then give the strip back
          if (!stopFadeStart) {
            stopFadeStart = now ? now : 1;
            stopFrom = leds;
          }
          const unsigned long el = now - stopFadeStart;
          if (el < kStopFadeMs) {
            const float x = el / (float)kStopFadeMs, f = (1.0f - x) * (1.0f - x);
            realtimeLock(2500, REALTIME_MODE_GENERIC);
            if (realtimeOverride == REALTIME_OVERRIDE_NONE) {
              const unsigned n = min((unsigned)stopFrom.size(), (unsigned)strip.getLengthTotal());
              for (unsigned i = 0; i < n; i++)
                setRealtimePixel(i, (uint8_t)(stopFrom[i].r * f), (uint8_t)(stopFrom[i].g * f), (uint8_t)(stopFrom[i].b * f), 0);
              e131NewData = true;
            }
          } else {
            stopFadeStart = 0;
            wasRunning = false;
            onStop();
          }
        } else {
          wasRunning = running;
          if (running) onStart();
          else onStop();
        }
      }
      bool release = false;
      const bool fresh = app->poll_leds(nowS(), leds, &release);  // always: keeps the stream's state current
      if (running) {
        if (fresh) {
          if (!driving) {
            driving = true;
            savedGammaOff = arlsDisableGammaCorrection;
            arlsDisableGammaCorrection = true;  // the pipeline already gives linear LED values
          }
          realtimeLock(2500, REALTIME_MODE_GENERIC);
          if (realtimeOverride == REALTIME_OVERRIDE_NONE) {
            const unsigned n = min((unsigned)leds.size(), (unsigned)strip.getLengthTotal());
            for (unsigned i = 0; i < n; i++) setRealtimePixel(i, leds[i].r, leds[i].g, leds[i].b, 0);
            e131NewData = true;  // WLED's own loop calls strip.show()
            fpsFrames++;
            lastOutMs = now ? now : 1;  // v0.44
          }
        }
        if (release && driving) {
          driving = false;
          arlsDisableGammaCorrection = savedGammaOff;
          if (realtimeMode) exitRealtime();
          lastBlack = 0;
        }
        if (!driving) {
          const ambi_bottom::Demo d = app->demo();
          if (d.on) {
            if (!demoApplied || !sameDemo(d, appliedDemo) || realtimeMode) applyDemo(d);
          } else {
            demoApplied = false;
            // no TV, no demo: the strip stays dark, and stays ours (WLED shows "realtime")
            if (now - lastBlack >= 1000) {
              lastBlack = now;
              realtimeLock(2500, REALTIME_MODE_GENERIC);
              if (realtimeOverride == REALTIME_OVERRIDE_NONE) {
                for (unsigned i = 0; i < strip.getLengthTotal(); i++) setRealtimePixel(i, 0, 0, 0, 0);
                e131NewData = true;
              }
            }
          }
        }
      }
      if (now - fpsWindowStart >= 1000) {
        outputFps = fpsFrames * 1000.0f / (now - fpsWindowStart);
        fpsFrames = 0;
        fpsWindowStart = now;
      }
      updatePageInfo(now);
    }
    {  // v0.44: Wi-Fi drops since boot (for «Διάγνωση»)
      const bool up = WiFi.isConnected();
      if (wifiWasUp && !up) wifiDrops++;
      wifiWasUp = up;
    }
    if (now - lastStatsSave >= 10000) {  // v0.41: the wear log goes to flash every few minutes while it changes
      lastStatsSave = now;
      app->save_stats_if_due();
    }
    if (ssdpListening && now - lastNotify >= 30000) {
      lastNotify = now;
      for (const std::string& n : app->ssdp_notifies()) {
        ssdp.writeTo(reinterpret_cast<const uint8_t*>(n.data()), n.size(), IPAddress(239, 255, 255, 250), 1900);
      }
    }
    if (now - lastStatusLed >= 150) {
      lastStatusLed = now;
      showIndicator();
    }
  }

  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    // v0.38: a link to our page in WLED's Info panel (WLED shows these values as HTML)
    {
      JsonArray link = user.createNestedArray(F("Ambilight"));
      // v0.40: with a small TV-with-glow icon (inline SVG: no font or file needed)
      link.add(F("<a class=\"btn btn-xs\" href=\"/ambi\" style=\"padding:4px 10px;display:inline-flex;align-items:center;gap:6px\">"
                 "<svg width=\"16\" height=\"14\" viewBox=\"0 0 24 20\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" "
                 "stroke-linecap=\"round\" aria-hidden=\"true\"><rect x=\"4\" y=\"3\" width=\"16\" height=\"11\" rx=\"1.5\"/>"
                 "<path d=\"M9 18h6M1 6v5M23 6v5\" opacity=\".7\"/></svg>Ambi page ↗</a>"));
    }
    JsonArray line = user.createNestedArray(F("AmbiBottom"));
    if (!enabled) { line.add(F("off")); return; }
    // v0.33: WLED's own current limiter (ABL) is the ONLY limit whenever this
    // usermod is not driving the strip (boot, presets, effects, after a stream
    // ends). Say so when it is off or set above what the build allows.
    {
      const unsigned abl = BusManager::ablMilliampsMax();
      // v0.49.2: the most the page ever gives WLED: the strip's share at the ceiling + WLED's MA_FOR_ESP
      const unsigned ceiling = (unsigned)(ambi_bottom::strip_budget_ma(ambi_bottom::max_budget_ma(), espSamePortMa) + 0.5) + MA_FOR_ESP;
      if (abl == 0 || abl > ceiling) {
        JsonArray warn = user.createNestedArray(F("AmbiBottom power"));
        String t = abl == 0 ? String(F("WARNING: WLED's current limiter (ABL) is OFF")) : String(F("WARNING: WLED's current limit is ")) + abl + F(" mA");
        t += F(" - set LED Preferences > Maximum current to at most ");
        t += ceiling;
        t += F(" mA: it is the only limit when the TV is not streaming");
        warn.add(t);
      }
    }
    if (!app) { line.add(F("waiting for the network")); return; }
    if (!app->running()) line.add(F("STOPPED - the strip is WLED's; Start on http://<this ESP>/ambi"));
    const ambi_bottom::Status s = app->status(nowS());
    if (s.tv_locked) line.add(F("TV LOCKED to LightGroup after a DTLS failure - restart the TV"));
    else if (s.old_session) line.add(F("TV streams from an old session - asking it to reconnect (~30 s)"));
    else if (s.link_remaining_ms > 0) line.add(F("pairing open - run Configure on the TV"));
    else if (s.tv_wants_pair) line.add(F("TV wants to pair - press the button"));
    else if (s.live) { String t = F("streaming "); t += String(s.fps, 0); t += F(" fps, "); t += String(s.current_ma, 0); t += F(" mA"); line.add(t); }
    else if (s.paired_users == 0) line.add(F("not paired - press the button, then Configure on the TV"));
    else line.add(F("paired, no stream"));
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[F("enabled")] = enabled;
    top[F("lamps")] = lamps;
    top[F("tvIp")] = tvIp;
    top[F("bridgeMac")] = bridgeMac;
    top[F("linkPin")] = linkPin;
    top[F("statusPin")] = statusPin;
    top[F("ledsUsed")] = ledsUsed;
    top[F("budgetMa")] = budgetMa;
    top[F("maPerLed")] = maPerLed;
    top[F("maScaleRPct")] = maScaleRPct;
    top[F("maScaleGPct")] = maScaleGPct;
    top[F("maScaleBPct")] = maScaleBPct;
    top[F("brightnessPct")] = brightnessPct;
    top[F("riseMs")] = riseMs;
    top[F("peakEmphasisPct")] = peakEmphasisPct;
    top[F("layout")] = layout;
    top[F("lampPositions")] = lampPositions;
    top[F("dtlsLimitS")] = dtlsLimitS;
    top[F("autoStart")] = autoStart;
    top[F("hueOnlyIp")] = hueOnlyIp;
    top[F("demoEnabled")] = demoEnabled;
    top[F("nightOn")] = nightOn;
    top[F("nightFrom")] = nightFrom;
    top[F("nightTo")] = nightTo;
    top[F("nightPct")] = nightPct;
    top[F("espSamePortMa")] = espSamePortMa;  // v0.49
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    bool ok = !top.isNull();
    ok &= getJsonValue(top[F("enabled")], enabled, true);
    ok &= getJsonValue(top[F("lamps")], lamps, (uint8_t)3);
    ok &= getJsonValue(top[F("tvIp")], tvIp, "");
    ok &= getJsonValue(top[F("bridgeMac")], bridgeMac, "");
    ok &= getJsonValue(top[F("linkPin")], linkPin, (int8_t)AMBIBOTTOM_LINK_PIN);
    ok &= getJsonValue(top[F("statusPin")], statusPin, (int8_t)AMBIBOTTOM_STATUS_PIN);
    ok &= getJsonValue(top[F("ledsUsed")], ledsUsed, (uint16_t)0);
    ok &= getJsonValue(top[F("budgetMa")], budgetMa, (uint16_t)450);
    ok &= getJsonValue(top[F("maPerLed")], maPerLed, (uint8_t)40);
    ok &= getJsonValue(top[F("maScaleRPct")], maScaleRPct, (uint16_t)100);
    ok &= getJsonValue(top[F("maScaleGPct")], maScaleGPct, (uint16_t)100);
    ok &= getJsonValue(top[F("maScaleBPct")], maScaleBPct, (uint16_t)100);
    ok &= getJsonValue(top[F("brightnessPct")], brightnessPct, (uint8_t)100);
    ok &= getJsonValue(top[F("riseMs")], riseMs, (uint16_t)120);
    ok &= getJsonValue(top[F("peakEmphasisPct")], peakEmphasisPct, (uint8_t)50);
    ok &= getJsonValue(top[F("layout")], layout, "tv");
    ok &= getJsonValue(top[F("lampPositions")], lampPositions, "");
    ok &= getJsonValue(top[F("dtlsLimitS")], dtlsLimitS, (uint8_t)3);
    ok &= getJsonValue(top[F("autoStart")], autoStart, false);
    ok &= getJsonValue(top[F("hueOnlyIp")], hueOnlyIp, "");
    ok &= getJsonValue(top[F("demoEnabled")], demoEnabled, false);
    ok &= getJsonValue(top[F("nightOn")], nightOn, false);
    ok &= getJsonValue(top[F("nightFrom")], nightFrom, (uint8_t)23);
    ok &= getJsonValue(top[F("nightTo")], nightTo, (uint8_t)7);
    ok &= getJsonValue(top[F("nightPct")], nightPct, (uint8_t)40);
    ok &= getJsonValue(top[F("espSamePortMa")], espSamePortMa, (uint16_t)0);
    if (espSamePortMa > 400) espSamePortMa = 400;
    if (nightFrom > 23) nightFrom = 23;
    if (nightTo > 23) nightTo = 23;
    if (nightPct < 5) nightPct = 5;
    if (nightPct > 100) nightPct = 100;
    applyNight();
    if (lamps != 5) lamps = 3;
    // v0.33: the same limits the pipeline applies (so the page shows what is really in force)
    if (budgetMa > (uint16_t)ambi_bottom::max_budget_ma()) budgetMa = (uint16_t)ambi_bottom::max_budget_ma();
    for (uint16_t* p : {&maScaleRPct, &maScaleGPct, &maScaleBPct}) {
      if (*p < 10) *p = 10;
      if (*p > 300) *p = 300;
    }
    if (brightnessPct > 100) brightnessPct = 100;
    if (peakEmphasisPct > 100) peakEmphasisPct = 100;
    if (dtlsLimitS < 1) dtlsLimitS = 1;
    // Strip settings apply at once; lamps / MAC / TV address / pins need a reboot (the bridge identity is fixed while it runs).
    if (app) app->set_strip(stripConfig());
    // v0.49.2: WLED's Maximum current follows too (Usermods page: a new limit or board share). Not
    // here: WLED holds its JSON lock while it calls this, and applyWledHw needs it - loop() pushes.
    if (hwSynced) hwPushPending = true;
    return ok;
  }

  void appendConfigData() override {
    oappend(SET_F("addInfo('AmbiBottom:lamps',1,'3 or 5 (reboot)');"));
    oappend(SET_F("addInfo('AmbiBottom:tvIp',1,'optional: only this address may pair (reboot)');"));
    oappend(SET_F("addInfo('AmbiBottom:bridgeMac',1,'empty = 00:17:88 + this ESP (reboot)');"));
    oappend(SET_F("addInfo('AmbiBottom:ledsUsed',1,'LEDs used from the start (0 = all of LED Preferences > Length); the rest stay dark');"));
    oappend(SET_F("addInfo('AmbiBottom:budgetMa',1,'mA, hard limit for the strip (the build allows at most 900)');"));
    oappend(SET_F("addInfo('AmbiBottom:maPerLed',1,'mA per LED at full white (40: 39 measured 2026-11-19; the page sets WLED LED current too)');"));
    oappend(SET_F("addInfo('AmbiBottom:enabled',1,'the page: http://&lt;this ESP&gt;/ambi (boots stopped unless autoStart)');"));
    oappend(SET_F("addInfo('AmbiBottom:autoStart',1,'Start by itself after boot (also on the page)');"));
    oappend(SET_F("addInfo('AmbiBottom:maScaleRPct',1,'% red draws of a third of white (100 = not measured)');"));
    oappend(SET_F("addInfo('AmbiBottom:maScaleGPct',1,'% green');"));
    oappend(SET_F("addInfo('AmbiBottom:maScaleBPct',1,'% blue');"));
    oappend(SET_F("addInfo('AmbiBottom:layout',1,'tv, inset or custom');"));
    oappend(SET_F("addInfo('AmbiBottom:lampPositions',1,'custom: 0..1 per lamp, e.g. 0,0.5,1');"));
    oappend(SET_F("addInfo('AmbiBottom:dtlsLimitS',1,'s the TV waits for the stream handshake');"));
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

const char AmbiBottomUsermod::_name[] PROGMEM = "AmbiBottom";
