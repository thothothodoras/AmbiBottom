// End-to-end self-test for the minimal Hue REST server: starts
// hue_rest::run_server() on its own thread and drives it with real HTTP
// requests over a plain socket (same idea as test_dtls_loopback.cpp - catch
// real bugs by actually running the code, not just reading it). This is
// what caught the double-wrapped {"success": {"": ...}} bug and the unsent
// placeholder responses during development; both are fixed now, but this
// test exists so a regression shows up the same way next time.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cjson/cJSON.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

#include "hue_rest.hpp"
#include "bridge_identity.hpp"
#include "pairing_store.hpp"
#include "settings.hpp"

namespace {

const uint16_t kTestPort = 18080;

struct HttpResult {
    int status = 0;
    std::string body;
    bool ok = false;
};

HttpResult http_request(const std::string& method, const std::string& path, const std::string& body,
                        uint16_t port = kTestPort) {
    HttpResult result;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return result;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return result;
    }
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!body.empty()) req << "Content-Length: " << body.size() << "\r\n";
    req << "Connection: close\r\n\r\n" << body;
    std::string out = req.str();
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(fd, out.data() + sent, out.size() - sent, 0);
        if (n <= 0) { close(fd); return result; }
        sent += static_cast<size_t>(n);
    }

    std::string buf;
    char chunk[4096];
    for (;;) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
    }
    close(fd);

    size_t header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos) return result;
    std::string head = buf.substr(0, header_end);
    result.body = buf.substr(header_end + 4);
    size_t sp1 = head.find(' ');
    size_t sp2 = head.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
        result.status = std::atoi(head.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
    }
    result.ok = true;
    return result;
}

[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}

}  // namespace

int main() {
    const std::string pairing_path = "/tmp/ambi_test_hue_rest_pairing.json";
    std::remove(pairing_path.c_str());
    pairing_store::ThreadSafeStore pairing(pairing_path);

    auto identity = bridge_identity::make_identity("192.0.2.50", kTestPort);

    const std::string settings_path = "/tmp/ambi_test_hue_rest_settings.json";
    std::remove(settings_path.c_str());
    settings::SharedSettings strip_settings(settings_path);

    // v0.22: a groups store, so the test can read back what the TV-created group looks like on disk.
    const std::string groups_path = "/tmp/ambi_test_hue_rest_groups.json";
    std::remove(groups_path.c_str());

    std::atomic<int> stream_active_calls{0};
    std::atomic<bool> last_active{false};
    std::string last_user, last_group;
    std::vector<std::string> last_lamp_ids;
    std::mutex cb_mu;

    std::thread server_thread([&] {
        hue_rest::Options opts;
        opts.port = kTestPort;
        opts.groups_store_path = groups_path;
        hue_rest::run_server(opts, identity, pairing, pairing_path,
                              [&](const std::string& user, const std::string& group, bool active,
                                  const std::vector<std::string>& lamp_ids) {
                                  std::lock_guard<std::mutex> lk(cb_mu);
                                  stream_active_calls++;
                                  last_active = active;
                                  last_user = user;
                                  last_group = group;
                                  last_lamp_ids = lamp_ids;
                              },
                              &strip_settings);
    });
    server_thread.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ---- description.xml ------------------------------------------------
    {
        auto r = http_request("GET", "/description.xml", "");
        if (!r.ok || r.status != 200) fail("GET /description.xml did not respond 200");
        if (r.body.find("<serialNumber>001788a45e01</serialNumber>") == std::string::npos) {
            fail("description.xml missing expected serialNumber");
        }
        if (r.body.find(identity.bridgeid) == std::string::npos) {
            // bridgeid isn't actually in description.xml in fakebridge.py - only
            // in SSDP/mDNS/config - so don't assert on it here; just a sanity scan.
        }
        std::fprintf(stderr, "[ok] GET /description.xml\n");
    }

    // ---- pairing ----------------------------------------------------------
    std::string username, clientkey;
    {
        auto r = http_request("POST", "/api",
                               R"({"devicetype":"ambi-bridge-cpp-test#unit","generateclientkey":true})");
        if (!r.ok || r.status != 200) fail("POST /api did not respond 200");
        cJSON* arr = cJSON_Parse(r.body.c_str());
        if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != 1) fail("POST /api: bad response shape");
        cJSON* entry = cJSON_GetArrayItem(arr, 0);
        cJSON* succ = cJSON_GetObjectItemCaseSensitive(entry, "success");
        if (!cJSON_IsObject(succ)) fail("POST /api: no \"success\" object");
        cJSON* user_item = cJSON_GetObjectItemCaseSensitive(succ, "username");
        cJSON* ck_item = cJSON_GetObjectItemCaseSensitive(succ, "clientkey");
        if (!cJSON_IsString(user_item) || !cJSON_IsString(ck_item)) fail("POST /api: missing username/clientkey");
        username = user_item->valuestring;
        clientkey = ck_item->valuestring;
        if (username.size() != 32 || clientkey.size() != 32) fail("POST /api: username/clientkey wrong length");
        cJSON_Delete(arr);

        // re-pairing the same devicetype must return the SAME username/clientkey
        auto r2 = http_request("POST", "/api",
                                R"({"devicetype":"ambi-bridge-cpp-test#unit","generateclientkey":true})");
        cJSON* arr2 = cJSON_Parse(r2.body.c_str());
        cJSON* succ2 = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr2, 0), "success");
        std::string username2 = cJSON_GetObjectItemCaseSensitive(succ2, "username")->valuestring;
        if (username2 != username) fail("re-pairing the same devicetype returned a DIFFERENT username");
        cJSON_Delete(arr2);
        std::fprintf(stderr, "[ok] POST /api pairing + idempotent re-pairing (username=%.8s...)\n", username.c_str());
    }

    // ---- the pairing actually persisted to disk, and the PSK it would
    // hand the DTLS server matches the clientkey the TV was given --------
    {
        pairing_store::Store reloaded = pairing_store::Store::load(pairing_path);
        auto u = reloaded.get(username);
        if (!u.has_value()) fail("pairing was not persisted to disk");
        std::string expected_upper = clientkey;
        std::transform(expected_upper.begin(), expected_upper.end(), expected_upper.begin(), ::toupper);
        std::string got_upper = u->clientkey_hex;
        std::transform(got_upper.begin(), got_upper.end(), got_upper.begin(), ::toupper);
        if (got_upper != expected_upper) fail("persisted clientkey doesn't match what the TV was handed");
        std::fprintf(stderr, "[ok] pairing persisted to disk, matches PSK the DTLS server would use\n");
    }

    // ---- config -------------------------------------------------------
    {
        auto r = http_request("GET", "/api/config", "");
        cJSON* c = cJSON_Parse(r.body.c_str());
        if (!c) fail("GET /api/config: not valid JSON");
        cJSON* mac = cJSON_GetObjectItemCaseSensitive(c, "mac");
        cJSON* bid = cJSON_GetObjectItemCaseSensitive(c, "bridgeid");
        if (!cJSON_IsString(mac) || std::string(mac->valuestring) != "00:17:88:a4:5e:01") fail("bad mac in config");
        if (!cJSON_IsString(bid) || std::string(bid->valuestring) != "001788FFFEA45E01") fail("bad bridgeid in config");
        cJSON_Delete(c);
        std::fprintf(stderr, "[ok] GET /api/config\n");
    }

    // ---- full dump: lights + groups ------------------------------------
    {
        auto r = http_request("GET", "/api/" + username, "");
        cJSON* root = cJSON_Parse(r.body.c_str());
        if (!root) fail("GET /api/<user>: not valid JSON");
        cJSON* lights = cJSON_GetObjectItemCaseSensitive(root, "lights");
        if (!cJSON_IsObject(lights) || cJSON_GetArraySize(lights) != 3) fail("expected 3 lights");
        cJSON* light1 = cJSON_GetObjectItemCaseSensitive(lights, "1");
        cJSON* name1 = cJSON_GetObjectItemCaseSensitive(light1, "name");
        if (!cJSON_IsString(name1) || std::string(name1->valuestring) != "Bottom-Left") fail("light 1 should be Bottom-Left");
        cJSON* caps = cJSON_GetObjectItemCaseSensitive(light1, "capabilities");
        cJSON* streaming = cJSON_GetObjectItemCaseSensitive(caps, "streaming");
        cJSON* renderer = cJSON_GetObjectItemCaseSensitive(streaming, "renderer");
        if (!cJSON_IsTrue(renderer)) fail("light 1 should have streaming.renderer=true");

        cJSON* groups = cJSON_GetObjectItemCaseSensitive(root, "groups");
        cJSON* g0 = cJSON_GetObjectItemCaseSensitive(groups, "0");
        if (!cJSON_IsObject(g0)) fail("expected group 0 to exist");
        cJSON* g2 = cJSON_GetObjectItemCaseSensitive(groups, "1");
        if (!cJSON_IsObject(g2)) fail("expected the pre-created Entertainment group 1 to exist (v0.31: ids start at 1)");
        cJSON* g2type = cJSON_GetObjectItemCaseSensitive(g2, "type");
        if (!cJSON_IsString(g2type) || std::string(g2type->valuestring) != "Entertainment") fail("group 2 should be Entertainment");
        cJSON* locations = cJSON_GetObjectItemCaseSensitive(g2, "locations");
        if (!cJSON_IsObject(locations) || cJSON_GetArraySize(locations) != 3) fail("group 2 should have 3 locations");
        cJSON_Delete(root);
        std::fprintf(stderr, "[ok] GET /api/<user> (3 lights with renderer=true, group 0 + Entertainment group 2)\n");
    }

    // ---- capabilities ---------------------------------------------------
    {
        auto r = http_request("GET", "/api/" + username + "/capabilities", "");
        cJSON* c = cJSON_Parse(r.body.c_str());
        cJSON* streaming = cJSON_GetObjectItemCaseSensitive(c, "streaming");
        cJSON* avail = cJSON_GetObjectItemCaseSensitive(streaming, "available");
        if (!cJSON_IsNumber(avail) || avail->valueint != 1) fail("capabilities.streaming.available should be 1");
        cJSON_Delete(c);
        std::fprintf(stderr, "[ok] GET /api/<user>/capabilities\n");
    }

    // ---- PUT light state --------------------------------------------------
    {
        auto r = http_request("PUT", "/api/" + username + "/lights/1/state",
                               R"({"on":true,"bri":100,"xy":[0.4,0.35]})");
        if (!r.ok || r.status != 200) fail("PUT lights/1/state did not respond 200");
        cJSON* arr = cJSON_Parse(r.body.c_str());
        if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != 3) fail("expected 3 ack entries (on, bri, xy)");
        cJSON_Delete(arr);
        std::fprintf(stderr, "[ok] PUT /api/<user>/lights/1/state\n");
    }

    // ---- stream.active toggling fires the callback -----------------------
    {
        auto r = http_request("PUT", "/api/" + username + "/groups/1", R"({"stream":{"active":true}})");
        if (!r.ok || r.status != 200) fail("PUT groups/1 stream.active=true did not respond 200");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            std::lock_guard<std::mutex> lk(cb_mu);
            if (stream_active_calls.load() != 1) fail("on_stream_active callback did not fire exactly once");
            if (!last_active.load()) fail("callback should report active=true");
            if (last_user != username) fail("callback got the wrong username");
            if (last_group != "1") fail("callback got the wrong group id");
            if (last_lamp_ids.size() != 3) fail("callback should report 3 lamp ids");
        }
        std::fprintf(stderr, "[ok] PUT stream.active=true fires on_stream_active with correct user/group/lamps\n");

        auto r2 = http_request("PUT", "/api/" + username + "/groups/1", R"({"stream":{"active":false}})");
        if (!r2.ok || r2.status != 200) fail("PUT groups/1 stream.active=false did not respond 200");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            std::lock_guard<std::mutex> lk(cb_mu);
            if (stream_active_calls.load() != 2) fail("on_stream_active callback did not fire a second time");
            if (last_active.load()) fail("callback should report active=false");
        }
        std::fprintf(stderr, "[ok] PUT stream.active=false fires on_stream_active with active=false\n");
    }

    // ---- unknown group id: TV's scan-stopping error shape -----------------
    {
        auto r = http_request("GET", "/api/" + username + "/groups/99", "");
        cJSON* arr = cJSON_Parse(r.body.c_str());
        if (!arr || !cJSON_IsArray(arr)) fail("expected an error array for unknown group");
        cJSON* err = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 0), "error");
        if (!cJSON_IsObject(err)) fail("expected {\"error\": ...} for unknown group 99");
        cJSON_Delete(arr);
        std::fprintf(stderr, "[ok] GET unknown group id returns the TV's expected error shape\n");
    }

    // ---- v0.22: a group the TV creates records when it was created --------
    {
        auto r = http_request("POST", "/api/" + username + "/groups",
                              R"({"type":"LightGroup","name":"Lounge test","lights":["1","2","3"]})");
        if (!r.ok || r.status != 200) fail("POST /groups did not respond 200");
        auto r2 = http_request("POST", "/api/" + username + "/groups",
                               R"({"type":"Entertainment","class":"TV","lights":["1","2","3"]})");
        if (!r2.ok || r2.status != 200) fail("POST /groups (Entertainment) did not respond 200");

        FILE* f = std::fopen(groups_path.c_str(), "rb");
        if (!f) fail("groups store was not written");
        std::string buf(1 << 16, '\0');
        buf.resize(std::fread(&buf[0], 1, buf.size(), f));
        std::fclose(f);
        cJSON* root = cJSON_Parse(buf.c_str());
        if (!root) fail("groups store is not valid JSON");
        int with_created = 0, without = 0;
        cJSON* g = nullptr;
        cJSON_ArrayForEach(g, root) {
            cJSON* c = cJSON_GetObjectItemCaseSensitive(g, "created");
            if (cJSON_IsString(c) && std::strlen(c->valuestring) == 19) {
                // "YYYY-MM-DD HH:MM:SS"
                const char* s = c->valuestring;
                if (s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':') {
                    fail("created is not formatted YYYY-MM-DD HH:MM:SS");
                }
                with_created++;
            } else {
                without++;
            }
        }
        cJSON_Delete(root);
        // v0.31: the Entertainment POST has the default group's type + lights,
        // so it is reused (no new group); only the LightGroup is new.
        if (r2.body.find("\"id\":\"1\"") == std::string::npos) fail("POST of an existing type+lights must return the existing id 1");
        if (r.body.find("\"id\":\"2\"") == std::string::npos) fail("the first new group must get id 2 (ids without holes)");
        if (with_created != 1) fail("the one TV-created group should have a created time");
        if (without < 1) fail("the built-in default group (1) should have no created time");
        std::fprintf(stderr, "[ok] groups created by the TV store their creation time (default group has none)\n");
    }

    // ---- v0.24: unknown user -> short config / error 1 -------------------
    {
        const std::string nobody = "0123456789abcdef0123456789abcdef";
        auto r = http_request("GET", "/api/" + nobody + "/config", "");
        cJSON* c = cJSON_Parse(r.body.c_str());
        if (!c || !cJSON_IsObject(c)) fail("unknown user GET /config: expected the short config object");
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(c, "bridgeid"))) fail("short config without bridgeid");
        if (cJSON_GetObjectItemCaseSensitive(c, "whitelist")) fail("short config must not list the whitelist");
        cJSON_Delete(c);
        for (const char* sub : {"", "/lights", "/groups", "/groups/1", "/capabilities"}) {
            auto e = http_request("GET", "/api/" + nobody + sub, "");
            cJSON* arr = cJSON_Parse(e.body.c_str());
            cJSON* err = arr && cJSON_IsArray(arr) ? cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 0), "error") : nullptr;
            cJSON* type = err ? cJSON_GetObjectItemCaseSensitive(err, "type") : nullptr;
            cJSON* desc = err ? cJSON_GetObjectItemCaseSensitive(err, "description") : nullptr;
            if (!cJSON_IsNumber(type) || type->valueint != 1 || !cJSON_IsString(desc) ||
                std::string(desc->valuestring) != "unauthorized user")
                fail((std::string("unknown user GET ") + sub + ": expected error 1 unauthorized user").c_str());
            cJSON_Delete(arr);
        }
        auto put = http_request("PUT", "/api/" + nobody + "/groups/1", R"({"stream":{"active":true}})");
        if (put.body.find("unauthorized user") == std::string::npos) fail("unknown user PUT must get error 1");
        {
            std::lock_guard<std::mutex> lk(cb_mu);
            if (stream_active_calls.load() != 2) fail("unknown user must not start a stream");
        }
        // the known user still gets everything
        auto ok = http_request("GET", "/api/" + username + "/config", "");
        if (ok.body.find("whitelist") == std::string::npos) fail("known user lost the full config");
        std::fprintf(stderr, "[ok] unknown user: /config short (no whitelist), everything else error 1; known user unchanged\n");
    }

    // ---- v0.24: pairing from another IP than the TV only with the link button
    {
        const uint16_t port2 = kTestPort + 1;
        const std::string path2 = "/tmp/ambi_test_hue_rest_pairing2.json";
        std::remove(path2.c_str());
        static pairing_store::ThreadSafeStore pairing2(path2);
        static auto identity2 = bridge_identity::make_identity("192.0.2.50", port2);
        std::thread t2([&] {
            hue_rest::Options o;
            o.port = port2;
            o.tv_ip = "192.0.2.77";  // the "TV" is elsewhere; this test talks from 127.0.0.1
            o.require_link_button = false;
            hue_rest::run_server(o, identity2, pairing2, path2, nullptr, nullptr);
        });
        t2.detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto no = http_request("POST", "/api", R"({"devicetype":"other#ip"})", port2);
        if (no.body.find("\"type\":101") == std::string::npos) fail("other IP without Pair must get error 101");
        pairing2.press_link_button(std::chrono::seconds(30));
        auto yes = http_request("POST", "/api", R"({"devicetype":"other#ip"})", port2);
        if (yes.body.find("\"success\"") == std::string::npos) fail("other IP with Pair open must pair");
        std::fprintf(stderr, "[ok] pairing from another IP: refused (101) without Pair, accepted with Pair open\n");
    }

    // ---- /ambi/settings: GET defaults, PUT a partial change, GET reflects
    // it, and it's actually persisted to disk (not just in memory) --------
    {
        auto r = http_request("GET", "/ambi/settings", "");
        cJSON* c = cJSON_Parse(r.body.c_str());
        cJSON* lc = cJSON_GetObjectItemCaseSensitive(c, "led_count");
        if (!cJSON_IsNumber(lc) || lc->valueint != 43) fail("default led_count should be 43");
        cJSON_Delete(c);

        auto r2 = http_request("PUT", "/ambi/settings", R"({"led_count":27,"diffuser":true})");
        if (!r2.ok || r2.status != 200) fail("PUT /ambi/settings did not respond 200");
        cJSON* c2 = cJSON_Parse(r2.body.c_str());
        cJSON* lc2 = cJSON_GetObjectItemCaseSensitive(c2, "led_count");
        cJSON* df2 = cJSON_GetObjectItemCaseSensitive(c2, "diffuser");
        cJSON* budget2 = cJSON_GetObjectItemCaseSensitive(c2, "current_budget_ma");
        if (!cJSON_IsNumber(lc2) || lc2->valueint != 27) fail("PUT should have changed led_count to 27");
        if (!cJSON_IsTrue(df2)) fail("PUT should have changed diffuser to true");
        if (!cJSON_IsNumber(budget2) || budget2->valuedouble != 450.0) {
            fail("PUT should leave current_budget_ma at its default (partial merge, not a full overwrite)");
        }
        cJSON_Delete(c2);

        color_pipeline::Settings reloaded = settings::load(settings_path);
        if (reloaded.led_count != 27 || !reloaded.diffuser) fail("PUT /ambi/settings did not persist to disk");
        std::fprintf(stderr, "[ok] GET/PUT /ambi/settings (partial merge + persisted to disk)\n");
    }


    // ---- v0.31: group ids 1..N, reuse, real DELETE, placeholder for gone ids
    {
        const std::string base = "/api/" + username;
        auto again = http_request("POST", base + "/groups", R"({"type":"LightGroup","lights":["3","2","1"]})");
        if (again.body.find("\"id\":\"2\"") == std::string::npos) fail("same type + same lights (any order) must reuse group 2");
        auto five = http_request("POST", base + "/groups", R"({"type":"LightGroup","lights":["1","2"]})");
        if (five.body.find("\"id\":\"3\"") == std::string::npos) fail("a different light set must get the next id 3");

        auto del = http_request("DELETE", base + "/groups/2", "");
        if (del.body.find("deleted") == std::string::npos) fail("DELETE /groups/2 must report deleted");
        auto list = http_request("GET", base + "/groups", "");
        cJSON* lj = cJSON_Parse(list.body.c_str());
        if (!lj || cJSON_GetObjectItemCaseSensitive(lj, "2")) fail("group 2 is still listed after DELETE");
        if (!cJSON_GetObjectItemCaseSensitive(lj, "1") || !cJSON_GetObjectItemCaseSensitive(lj, "3")) fail("groups 1 and 3 must survive the DELETE");
        cJSON_Delete(lj);
        auto del2 = http_request("DELETE", base + "/groups/2", "");
        if (del2.body.find("\"error\"") == std::string::npos) fail("DELETE of a group that is gone must be an error");

        // The TV stops its 1,2,3.. search at the first "error": a deleted id
        // in the middle must not be one, and must not look like its lamps.
        auto gone = http_request("GET", base + "/groups/2", "");
        if (gone.body.find("error") != std::string::npos) fail("a deleted id below the highest one must not answer with an error");
        if (gone.body.find("\"type\"") == std::string::npos || gone.body.find("\"lights\":[]") == std::string::npos) fail("placeholder needs type and empty lights");
        auto beyond = http_request("GET", base + "/groups/4", "");
        if (beyond.body.find("\"error\"") == std::string::npos) fail("an id above the highest one must still be an error (ends the TV's search)");

        auto refill = http_request("POST", base + "/groups", R"({"type":"LightGroup","lights":["1","2","3"]})");
        if (refill.body.find("\"id\":\"2\"") == std::string::npos) fail("a new group must take the lowest free id (2)");
        auto putghost = http_request("PUT", base + "/groups/9", R"({"lights":["1"]})");
        if (putghost.body.find("\"error\"") == std::string::npos) fail("PUT lights on an unknown group must not create it");
        std::fprintf(stderr, "[ok] v0.31 groups: reuse by type+lights, ids without holes, real DELETE, placeholder for gone ids\n");
    }

    // ---- v0.31: what the TV checks before it asks for the stream ---------
    {
        const std::string base = "/api/" + username;
        auto cfg = http_request("GET", base + "/config", "");
        cJSON* c = cJSON_Parse(cfg.body.c_str());
        cJSON* model = cJSON_GetObjectItemCaseSensitive(c, "modelid");
        cJSON* api = cJSON_GetObjectItemCaseSensitive(c, "apiversion");
        if (!cJSON_IsString(model) || std::string(model->valuestring) != "BSB002") fail("/config modelid must be BSB002");
        int major = 0, minor = 0;
        if (!cJSON_IsString(api) || std::sscanf(api->valuestring, "%d.%d", &major, &minor) != 2 ||
            !(major > 1 || minor >= 22)) fail("/config apiversion must be >= 1.22");
        cJSON_Delete(c);
        auto caps = http_request("GET", base + "/capabilities", "");
        cJSON* cj = cJSON_Parse(caps.body.c_str());
        cJSON* av = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cj, "streaming"), "available");
        if (!cJSON_IsNumber(av) || av->valueint <= 0) fail("/capabilities streaming.available must be > 0");
        if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(cj, "groups"))) fail("/capabilities must have groups");
        cJSON_Delete(cj);

        // Two strangers connect and say nothing (until v0.30 each held the
        // whole server for 2 s). The TV's test: 10 GET /lights/1, 300 ms each.
        int idle1 = socket(AF_INET, SOCK_STREAM, 0), idle2 = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kTestPort);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(idle1, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
            connect(idle2, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) fail("could not open the idle connections");
        double worst_ms = 0;
        for (int i = 0; i < 10; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            auto r = http_request("GET", base + "/lights/1", "");
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst_ms = std::max(worst_ms, ms);
            if (!r.ok || r.status != 200 || r.body.find("\"state\"") == std::string::npos) fail("GET /lights/1 must be 200 with the light");
            if (ms >= 300.0) fail("GET /lights/1 took " + std::to_string(ms) + " ms with idle connections open (TV limit 300 ms)");
        }
        close(idle1);
        close(idle2);
        for (const char* lid : {"2", "3", "9", "abc"}) {
            auto r = http_request("GET", base + "/lights/" + lid, "");
            if (!r.ok || r.status != 200) fail(std::string("GET /lights/") + lid + " must answer HTTP 200 (the TV counts anything else as no connection)");
        }
        std::fprintf(stderr, "[ok] v0.31 TV pre-stream checks: BSB002, apiversion >= 1.22, streaming.available > 0, "
                             "10x GET /lights/1 worst %.1f ms with 2 idle connections, /lights/N always 200\n", worst_ms);
    }

    // ---- v0.31: an older groups store (ids from 2, many copies) is tidied
    {
        const uint16_t port3 = kTestPort + 2;
        const std::string gpath3 = "/tmp/ambi_test_hue_rest_groups3.json";
        const std::string ppath3 = "/tmp/ambi_test_hue_rest_pairing3.json";
        std::remove(ppath3.c_str());
        {
            FILE* f = std::fopen(gpath3.c_str(), "wb");
            const char* old_store = R"({
 "2":{"name":"TV","type":"Entertainment","class":"TV","lights":["1","2","3"],"locations":{}},
 "3":{"name":"Group 3","type":"LightGroup","lights":["1","2","3"],"locations":{}},
 "4":{"name":"TV","type":"Entertainment","class":"TV","lights":["1","2","3"],"created":"2026-11-01 21:06:10","locations":{}},
 "5":{"name":"Group 5","type":"LightGroup","lights":["1","2","3"],"created":"2026-11-01 21:06:20","locations":{}},
 "6":{"name":"TV","type":"Entertainment","class":"TV","lights":["1","2","3","4","5"],"created":"2026-11-05 10:00:00","locations":{}},
 "7":{"name":"TV","type":"Entertainment","class":"TV","lights":["1","2","3"],"created":"2026-11-06 09:00:00","locations":{}}
})";
            std::fwrite(old_store, 1, std::strlen(old_store), f);
            std::fclose(f);
        }
        static pairing_store::ThreadSafeStore pairing3(ppath3);
        static auto identity3 = bridge_identity::make_identity("192.0.2.50", port3);
        std::thread t3([&] {
            hue_rest::Options o;
            o.port = port3;
            o.groups_store_path = gpath3;
            hue_rest::run_server(o, identity3, pairing3, ppath3, nullptr, nullptr);
        });
        t3.detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto pr = http_request("POST", "/api", R"({"devicetype":"tidy#test"})", port3);
        size_t up = pr.body.find("\"username\":\"");
        if (up == std::string::npos) fail("pairing on the tidy-test server failed");
        const std::string u3 = pr.body.substr(up + 12, 32);
        auto list = http_request("GET", "/api/" + u3 + "/groups", "", port3);
        cJSON* lj = cJSON_Parse(list.body.c_str());
        if (!lj) fail("tidy test: /groups is not JSON");
        int n = 0;
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, lj) if (it->string && std::string(it->string) != "0") ++n;  // "0" = all lights
        cJSON* g1 = cJSON_GetObjectItemCaseSensitive(lj, "1");
        cJSON* g1type = cJSON_GetObjectItemCaseSensitive(g1, "type");
        if (n != 3) fail("6 stored groups with 3 distinct type+lights must become 3, got " + std::to_string(n));
        if (!cJSON_IsString(g1type) || std::string(g1type->valuestring) != "Entertainment") fail("group 1 must be an Entertainment group after tidying");
        if (!cJSON_GetObjectItemCaseSensitive(lj, "2") || !cJSON_GetObjectItemCaseSensitive(lj, "3")) fail("tidied ids must be 1..3");
        cJSON_Delete(lj);
        // the id the TV may still remember (7) and the old ones: no error; 8: error
        for (const char* gid : {"4", "5", "6", "7"}) {
            auto r = http_request("GET", "/api/" + u3 + "/groups/" + gid, "", port3);
            if (r.body.find("error") != std::string::npos) fail(std::string("old id ") + gid + " must not answer with an error after tidying");
        }
        auto r8 = http_request("GET", "/api/" + u3 + "/groups/8", "", port3);
        if (r8.body.find("\"error\"") == std::string::npos) fail("id 8 never existed: must be an error");
        // what the TV does next: it finds its lamps at id 1 and POSTs nothing;
        // if it did POST, it gets the existing group back
        auto p = http_request("POST", "/api/" + u3 + "/groups", R"({"type":"Entertainment","class":"TV","lights":["1","2","3"]})", port3);
        if (p.body.find("\"id\":\"1\"") == std::string::npos && p.body.find("\"id\":\"2\"") == std::string::npos) fail("POST after tidying must reuse an existing group");
        std::fprintf(stderr, "[ok] v0.31 old groups store tidied: 6 -> 3 groups, ids 1..3, Entertainment first, old ids 4..7 are placeholders, 8 is an error\n");
    }

    std::printf("ALL HUE_REST TESTS PASSED\n");
    return 0;
}
