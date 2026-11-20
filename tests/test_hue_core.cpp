// hue_core::Bridge on its own: no sockets, no files, a pairing store in
// memory - the way the ESP32 usermod drives it. The same behaviour is tested
// over real sockets in test_hue_rest.cpp (the Linux front end).
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "hue_core.hpp"

namespace {

[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

class MemPairing : public hue_core::Pairing {
public:
    bool get(const std::string& u) override { return users.count(u) != 0; }
    std::pair<std::string, hue_core::PairedUser> pair(const std::string& devicetype, bool key) override {
        const std::string name = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" + std::to_string(10 + users.size());
        users[name] = devicetype;
        return {name, hue_core::PairedUser{key ? "00112233445566778899aabbccddeeff" : ""}};
    }
    std::vector<std::pair<std::string, std::string>> users_with_devicetype() override {
        return {users.begin(), users.end()};
    }
    long link_remaining_ms() override { return link_ms; }
    void note_pair_refused() override { ++refused; }
    std::map<std::string, std::string> users;
    long link_ms = 0;
    int refused = 0;
};

}  // namespace

int main() {
    const auto identity = bridge_identity::make_identity("192.0.2.50", 80);
    MemPairing pairing;
    std::string stored;  // the "NVS"
    int saves = 0, stream_calls = 0, lightgroup_calls = 0;
    std::string last_stream;

    hue_core::Options o;
    o.require_link_button = true;
    o.load_groups_text = [&] { return stored; };
    o.save_groups_text = [&](const std::string& t) {
        stored = t;
        ++saves;
    };
    o.on_stream_active = [&](const std::string&, const std::string& gid, bool active, const std::vector<std::string>&) {
        ++stream_calls;
        last_stream = gid + (active ? ":on" : ":off");
    };
    o.on_lightgroup_used = [&] { ++lightgroup_calls; };
    auto bridge = std::make_unique<hue_core::Bridge>(o, identity, pairing);
    auto req = [&](const char* m, const std::string& p, const std::string& b = "") {
        return bridge->handle(m, p, b, "192.0.2.10", "test");
    };

    // ---- description.xml, short config -----------------------------------
    auto d = req("GET", "/description.xml");
    if (d.status != 200 || d.content_type != "text/xml" || !has(d.body, "<modelNumber>BSB002</modelNumber>") ||
        !has(d.body, "192.0.2.50:80")) fail("description.xml");
    auto c0 = req("GET", "/api/config");
    if (!has(c0.body, "\"modelid\":\"BSB002\"") || has(c0.body, "whitelist")) fail("short /api/config");
    if (req("GET", "/nothing").status != 404) fail("unknown path must be 404");

    // ---- pairing only with the link button --------------------------------
    auto p0 = req("POST", "/api", R"({"devicetype":"tv#1","generateclientkey":true})");
    if (!has(p0.body, "\"type\":101") || pairing.refused != 1) fail("pairing without the button must be error 101");
    pairing.link_ms = 30000;
    auto p1 = req("POST", "/api", R"({"devicetype":"tv#1","generateclientkey":true})");
    if (!has(p1.body, "\"username\":\"") || !has(p1.body, "\"clientkey\":\"00112233445566778899AABBCCDDEEFF\""))
        fail("pairing with the button open: username + upper-case clientkey");
    pairing.link_ms = 0;
    const std::string u = pairing.users.begin()->first;
    const std::string base = "/api/" + u;
    std::fprintf(stderr, "[ok] description.xml, short config, pairing needs the link button\n");

    // ---- what the TV checks -------------------------------------------------
    auto cfg = req("GET", base + "/config");
    if (!has(cfg.body, "\"modelid\":\"BSB002\"") || !has(cfg.body, "\"apiversion\":\"1.67.0\"") || !has(cfg.body, "whitelist"))
        fail("full config");
    auto caps = req("GET", base + "/capabilities");
    if (!has(caps.body, "\"streaming\":{\"available\":1") || !has(caps.body, "\"groups\"")) fail("capabilities");
    for (const char* lid : {"1", "2", "3", "9"}) {
        if (req("GET", base + "/lights/" + lid).status != 200) fail("/lights/N must always be 200");
    }
    auto l1 = req("GET", base + "/lights/1");
    if (!has(l1.body, "\"renderer\":true") || !has(l1.body, "Extended color light")) fail("light 1 must be a streaming renderer");

    // ---- groups: ids from 1, reuse, DELETE, placeholder ---------------------
    if (!has(req("GET", base + "/groups/1").body, "\"type\":\"Entertainment\"")) fail("default group must be Entertainment at id 1");
    auto e = req("POST", base + "/groups", R"({"type":"Entertainment","class":"TV","lights":["1","2","3"]})");
    if (!has(e.body, "\"id\":\"1\"")) fail("same type+lights must reuse id 1");
    auto lg = req("POST", base + "/groups", R"({"type":"LightGroup","lights":["1","2","3"]})");
    if (!has(lg.body, "\"id\":\"2\"") || lightgroup_calls != 1) fail("LightGroup must get id 2 and be reported");
    auto lg2 = req("POST", base + "/groups", R"({"type":"LightGroup","lights":["3","1","2"]})");
    if (!has(lg2.body, "\"id\":\"2\"")) fail("same lights in another order must reuse id 2");
    auto third = req("POST", base + "/groups", R"({"type":"LightGroup","lights":["1"]})");
    if (!has(third.body, "\"id\":\"3\"")) fail("next new group must be id 3");
    if (!has(req("DELETE", base + "/groups/2").body, "deleted")) fail("DELETE /groups/2");
    if (has(req("GET", base + "/groups").body, "\"2\":{\"name\"")) fail("group 2 still listed after DELETE");
    auto gone = req("GET", base + "/groups/2");
    if (has(gone.body, "error") || !has(gone.body, "\"lights\":[]")) fail("a gone id must answer the empty placeholder, not an error");
    if (!has(req("GET", base + "/groups/4").body, "\"error\"")) fail("an id above the highest must be an error");
    if (!has(req("DELETE", base + "/groups/2").body, "\"error\"")) fail("DELETE of a gone group must be an error");
    if (!has(req("PUT", base + "/groups/9", R"({"lights":["1"]})").body, "\"error\"")) fail("PUT lights must not create a group");
    std::fprintf(stderr, "[ok] groups: ids from 1, reuse by type+lights, real DELETE, placeholder, no implicit create\n");

    // ---- stream.active ------------------------------------------------------
    auto on = req("PUT", base + "/groups/1", R"({"stream":{"active":true}})");
    if (!has(on.body, "\"/groups/1/stream/active\":true") || stream_calls != 1 || last_stream != "1:on") fail("stream.active=true");
    if (!has(req("GET", base + "/groups/1").body, "\"active\":true")) fail("group 1 must report the stream as active");
    req("PUT", base + "/groups/1", R"({"stream":{"active":false}})");
    if (stream_calls != 2 || last_stream != "1:off") fail("stream.active=false");

    // ---- unknown user ---------------------------------------------------------
    const std::string nobody = "0123456789abcdef0123456789abcdef";
    const int refused_before = pairing.refused;
    if (!has(req("GET", "/api/" + nobody + "/lights").body, "\"type\":1")) fail("unknown user must get error 1");
    if (has(req("GET", "/api/" + nobody + "/config").body, "whitelist")) fail("unknown user gets only the short config");
    req("PUT", "/api/" + nobody + "/groups/1", R"({"stream":{"active":true}})");
    if (stream_calls != 2) fail("unknown user must not start a stream");
    if (pairing.refused != refused_before + 1) fail("a 32-hex unknown name raises 'TV wants to pair' once, not per request");
    std::fprintf(stderr, "[ok] stream.active callbacks; unknown user: error 1, short config, lost-pairing noted once\n");

    // ---- persistence: what was saved is what a new bridge (after a reboot) has
    if (saves < 3 || stored.empty()) fail("group changes must be saved");
    bridge = std::make_unique<hue_core::Bridge>(o, identity, pairing);
    if (!has(req("GET", base + "/groups/1").body, "\"type\":\"Entertainment\"")) fail("group 1 lost across a restart");
    if (!has(req("GET", base + "/groups/3").body, "\"lights\":[\"1\"]")) fail("group 3 lost across a restart");
    if (has(req("GET", base + "/groups/2").body, "error")) fail("the gone id 2 must still be a placeholder after a restart");
    if (!has(req("POST", base + "/groups", R"({"type":"LightGroup","lights":["2"]})").body, "\"id\":\"2\""))
        fail("after a restart a new group takes the lowest free id");
    std::fprintf(stderr, "[ok] the saved text restores the same groups, holes and all, in a new Bridge\n");

    std::printf("ALL HUE_CORE TESTS PASSED\n");
    return 0;
}
