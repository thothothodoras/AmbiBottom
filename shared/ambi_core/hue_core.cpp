// See hue_core.hpp. The request handling below is the v0.31 body of
// hue_rest::run_server, moved here unchanged (v0.32).
#include "hue_core.hpp"

#if __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#include <cJSON.h>  // ESP-IDF "json" component
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <set>

namespace hue_core {

namespace {

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

// ---- in-memory bridge state (LIGHTS/GROUPS) --------------------------------
// Single-threaded: only this function's own accept-loop ever touches these
// (unlike the pairing store, which the DTLS thread also reads/writes via
// ThreadSafeStore's own lock).
struct Light {
    std::string name;
    bool on = true;
    int bri = 254;
    double x = 0.3127, y = 0.329;
};

struct Group {
    std::string name = "TV";
    std::vector<std::string> lights;
    std::string type = "LightGroup";
    std::string klass;
    std::string created;  // v0.22: local time the TV created it ("" = unknown / default / older store)
    bool stream_active = false;
    std::string stream_owner;
    std::map<std::string, std::array<double, 3>> locations;
};

// ---- group persistence (survives restarts, unlike lights/groups' default
// in-memory state) - same JSON-file-with-atomic-rename pattern as
// pairing_store/settings. stream_active/stream_owner are intentionally
// never written or read back: a restart always comes back with streaming
// off, and the TV re-requests stream.active=true itself once it finds the
// group it remembers again - that's the whole point of persisting this.
//
// v0.31: group ids are 1..N without holes, because the TV looks for its
// group by asking /groups/1, /groups/2, ... and stops at the first "error"
// (HueControl.getHueGroup). `high_water` is the highest id ever handed out:
// an id up to it that no longer exists (deleted, or merged away by
// normalize_groups) is answered with an empty placeholder instead of an
// error, so the scan walks past it - and, more important, so a group id the
// TV still remembers never gets an error (that costs the TV a whole session
// without a stream). Stored in the same file as "_high_water" (a number, which
// older versions skip).
std::string groups_to_text(const std::map<std::string, Group>& groups, int high_water);
std::map<std::string, Group> groups_from_text(const std::string& buf, int* high_water);

// "Entertainment|1,2,3": two groups with the same key are the same group to the TV.
std::string group_key(const Group& g) {
    std::vector<std::string> ls = g.lights;
    std::sort(ls.begin(), ls.end());
    std::string k = g.type + "|";
    for (const auto& l : ls) k += l + ",";
    return k;
}

bool is_group_number(const std::string& id) {
    return !id.empty() && id.size() <= 4 && id[0] != '0' &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Smallest id >= 1 not in use (fills the holes left by deleted groups first).
std::string lowest_free_group_id(const std::map<std::string, Group>& groups) {
    for (int n = 1;; ++n) {
        std::string id = std::to_string(n);
        if (!groups.count(id)) return id;
    }
}

// Brings an older store (ids from 2, one new group per TV session, v0.30 and
// before) to the v0.31 shape: one group per (type, lights), newest copy kept,
// Entertainment groups first, ids 1..N. Returns true when anything changed.
bool normalize_groups(std::map<std::string, Group>& groups, int& high_water) {
    std::vector<std::pair<int, std::string>> ids;  // numeric id, key
    for (const auto& [gid, g] : groups) {
        (void)g;
        ids.emplace_back(is_group_number(gid) ? std::stoi(gid) : 0, gid);
    }
    std::sort(ids.begin(), ids.end());
    int old_max = 0;
    bool contiguous = true;
    std::set<std::string> seen;
    bool duplicates = false;
    for (size_t k = 0; k < ids.size(); ++k) {
        old_max = std::max(old_max, ids[k].first);
        if (ids[k].first != static_cast<int>(k) + 1) contiguous = false;
        if (!seen.insert(group_key(groups.at(ids[k].second))).second) duplicates = true;
    }
    high_water = std::max(high_water, old_max);
    if (contiguous && !duplicates) return false;

    std::vector<Group> kept;
    std::set<std::string> have;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {  // newest id first: its copy wins
        Group& g = groups.at(it->second);
        if (have.insert(group_key(g)).second) kept.push_back(std::move(g));
    }
    std::reverse(kept.begin(), kept.end());  // back to oldest first
    std::stable_partition(kept.begin(), kept.end(), [](const Group& g) { return g.type == "Entertainment"; });
    groups.clear();
    int n = 0;
    for (auto& g : kept) groups[std::to_string(++n)] = std::move(g);
    high_water = std::max(high_water, n);
    return true;
}

std::map<std::string, std::array<double, 3>> bottom_locations(const std::vector<std::string>& lights) {
    std::map<std::string, std::array<double, 3>> out;
    int n = std::max(static_cast<int>(lights.size()) - 1, 1);
    for (size_t k = 0; k < lights.size(); ++k) {
        double xpos = std::round((-0.8 + 1.6 * double(k) / double(n)) * 100.0) / 100.0;
        out[lights[k]] = {xpos, 1.0, -1.0};
    }
    return out;
}

std::string groups_to_text(const std::map<std::string, Group>& groups, int high_water) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "_high_water", high_water);
    for (auto& [gid, g] : groups) {
        cJSON* go = cJSON_CreateObject();
        cJSON_AddStringToObject(go, "name", g.name.c_str());
        cJSON_AddStringToObject(go, "type", g.type.c_str());
        if (!g.klass.empty()) cJSON_AddStringToObject(go, "class", g.klass.c_str());
        if (!g.created.empty()) cJSON_AddStringToObject(go, "created", g.created.c_str());
        cJSON* lj = cJSON_CreateArray();
        for (auto& l : g.lights) cJSON_AddItemToArray(lj, cJSON_CreateString(l.c_str()));
        cJSON_AddItemToObject(go, "lights", lj);
        cJSON* locs = cJSON_CreateObject();
        for (auto& [lid, pos] : g.locations) {
            cJSON* arr = cJSON_CreateArray();
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(pos[0]));
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(pos[1]));
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(pos[2]));
            cJSON_AddItemToObject(locs, lid.c_str(), arr);
        }
        cJSON_AddItemToObject(go, "locations", locs);
        cJSON_AddItemToObject(root, gid.c_str(), go);
    }
    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    std::string out = text ? text : "{}";
    if (text) cJSON_free(text);
    return out;
}

std::map<std::string, Group> groups_from_text(const std::string& buf, int* high_water) {
    std::map<std::string, Group> out;
    if (high_water) *high_water = 0;
    if (buf.empty()) return out;

    cJSON* root = cJSON_ParseWithLength(buf.c_str(), buf.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return out;  // malformed -> empty, never crash
    }
    if (high_water) {
        cJSON* hw = cJSON_GetObjectItemCaseSensitive(root, "_high_water");
        if (cJSON_IsNumber(hw) && hw->valuedouble >= 0 && hw->valuedouble <= 9999) *high_water = hw->valueint;
    }
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, root) {
        if (!entry->string || !cJSON_IsObject(entry)) continue;
        Group g;
        cJSON* name_item = cJSON_GetObjectItemCaseSensitive(entry, "name");
        if (cJSON_IsString(name_item) && name_item->valuestring) g.name = name_item->valuestring;
        cJSON* type_item = cJSON_GetObjectItemCaseSensitive(entry, "type");
        if (cJSON_IsString(type_item) && type_item->valuestring) g.type = type_item->valuestring;
        cJSON* class_item = cJSON_GetObjectItemCaseSensitive(entry, "class");
        if (cJSON_IsString(class_item) && class_item->valuestring) g.klass = class_item->valuestring;
        cJSON* created_item = cJSON_GetObjectItemCaseSensitive(entry, "created");
        if (cJSON_IsString(created_item) && created_item->valuestring) g.created = created_item->valuestring;
        cJSON* lights_item = cJSON_GetObjectItemCaseSensitive(entry, "lights");
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, lights_item) {
            if (cJSON_IsString(item)) g.lights.push_back(item->valuestring);
        }
        cJSON* locs = cJSON_GetObjectItemCaseSensitive(entry, "locations");
        cJSON* loc = nullptr;
        cJSON_ArrayForEach(loc, locs) {
            if (!loc->string || !cJSON_IsArray(loc) || cJSON_GetArraySize(loc) != 3) continue;
            g.locations[loc->string] = {cJSON_GetArrayItem(loc, 0)->valuedouble,
                                         cJSON_GetArrayItem(loc, 1)->valuedouble,
                                         cJSON_GetArrayItem(loc, 2)->valuedouble};
        }
        // stream_active/stream_owner stay at their default (false/empty) -
        // never trust a persisted "active" stream across a restart.
        out[entry->string] = std::move(g);
    }
    cJSON_Delete(root);
    return out;
}

cJSON* make_light_json(int i, const std::string& name, const std::string& serial, const Light& l) {
    cJSON* root = cJSON_CreateObject();
    cJSON* state = cJSON_CreateObject();
    cJSON_AddBoolToObject(state, "on", l.on);
    cJSON_AddNumberToObject(state, "bri", l.bri);
    cJSON_AddNumberToObject(state, "hue", 0);
    cJSON_AddNumberToObject(state, "sat", 0);
    cJSON_AddStringToObject(state, "effect", "none");
    cJSON* xy = cJSON_CreateArray();
    cJSON_AddItemToArray(xy, cJSON_CreateNumber(l.x));
    cJSON_AddItemToArray(xy, cJSON_CreateNumber(l.y));
    cJSON_AddItemToObject(state, "xy", xy);
    cJSON_AddNumberToObject(state, "ct", 153);
    cJSON_AddStringToObject(state, "alert", "none");
    cJSON_AddStringToObject(state, "colormode", "xy");
    cJSON_AddStringToObject(state, "mode", "homeautomation");
    cJSON_AddBoolToObject(state, "reachable", true);
    cJSON_AddItemToObject(root, "state", state);

    cJSON* swupdate = cJSON_CreateObject();
    cJSON_AddStringToObject(swupdate, "state", "noupdates");
    cJSON_AddStringToObject(swupdate, "lastinstall", now_iso().c_str());
    cJSON_AddItemToObject(root, "swupdate", swupdate);

    cJSON_AddStringToObject(root, "type", "Extended color light");
    cJSON_AddStringToObject(root, "name", name.c_str());
    cJSON_AddStringToObject(root, "modelid", "LCT015");
    cJSON_AddStringToObject(root, "manufacturername", "Signify Netherlands B.V.");
    cJSON_AddStringToObject(root, "productname", "Hue color lamp");

    cJSON* caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "certified", true);
    cJSON* control = cJSON_CreateObject();
    cJSON_AddNumberToObject(control, "mindimlevel", 1000);
    cJSON_AddNumberToObject(control, "maxlumen", 806);
    cJSON_AddStringToObject(control, "colorgamuttype", "C");
    cJSON* gamut = cJSON_CreateArray();
    double pts[3][2] = {{0.6915, 0.3083}, {0.17, 0.7}, {0.1532, 0.0475}};
    for (auto& p : pts) {
        cJSON* pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(p[0]));
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(p[1]));
        cJSON_AddItemToArray(gamut, pair);
    }
    cJSON_AddItemToObject(control, "colorgamut", gamut);
    cJSON* ct = cJSON_CreateObject();
    cJSON_AddNumberToObject(ct, "min", 153);
    cJSON_AddNumberToObject(ct, "max", 500);
    cJSON_AddItemToObject(control, "ct", ct);
    cJSON_AddItemToObject(caps, "control", control);
    cJSON* streaming = cJSON_CreateObject();
    cJSON_AddBoolToObject(streaming, "renderer", true);  // this bridge is always stream-capable
    cJSON_AddBoolToObject(streaming, "proxy", false);
    cJSON_AddItemToObject(caps, "streaming", streaming);
    cJSON_AddItemToObject(root, "capabilities", caps);

    cJSON* config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "archetype", "sultanbulb");
    cJSON_AddStringToObject(config, "function", "mixed");
    cJSON_AddStringToObject(config, "direction", "omnidirectional");
    cJSON_AddItemToObject(root, "config", config);

    char uid[64];
    std::string s = serial.size() >= 12 ? serial : std::string(12, '0');
    std::snprintf(uid, sizeof(uid), "00:17:88:01:%s:%s:%s:%02x-0b",
                  s.substr(6, 2).c_str(), s.substr(8, 2).c_str(), s.substr(10, 2).c_str(), i);
    cJSON_AddStringToObject(root, "uniqueid", uid);
    cJSON_AddStringToObject(root, "swversion", "1.122.2");
    return root;
}

cJSON* make_error(const std::string& path, int type, const std::string& desc) {
    cJSON* arr = cJSON_CreateArray();
    cJSON* obj = cJSON_CreateObject();
    cJSON* err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "type", type);
    cJSON_AddStringToObject(err, "address", path.c_str());
    cJSON_AddStringToObject(err, "description", desc.c_str());
    cJSON_AddItemToObject(obj, "error", err);
    cJSON_AddItemToArray(arr, obj);
    return arr;
}

// [{"success": value}], the shape every Hue PUT/POST ack uses. `value` can
// be an object ({"/groups/2/action": "ok"}), an empty object ({}), or a
// bare string (DELETE's "<path> deleted") - whatever the real bridge's
// "success" value actually is for that call. Takes ownership of `value`.
cJSON* wrap_success(cJSON* value) {
    cJSON* arr = cJSON_CreateArray();
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddItemToObject(obj, "success", value);
    cJSON_AddItemToArray(arr, obj);
    return arr;
}

// [{"success": {key: value}}] - for the common case of a single-key ack
// object, e.g. {"success": {"id": "2"}}. Takes ownership of `value`.
cJSON* make_success(const std::string& key, cJSON* value) {
    cJSON* succ = cJSON_CreateObject();
    cJSON_AddItemToObject(succ, key.c_str(), value);
    return wrap_success(succ);
}

// Every group carries an "action" object on a real bridge and in the old
// Python bridge's _groups(); the C++ version omitted it until v8.
cJSON* make_group_action() {
    cJSON* a = cJSON_CreateObject();
    cJSON_AddBoolToObject(a, "on", true);
    cJSON_AddNumberToObject(a, "bri", 254);
    cJSON* xy = cJSON_CreateArray();
    cJSON_AddItemToArray(xy, cJSON_CreateNumber(0.3127));
    cJSON_AddItemToArray(xy, cJSON_CreateNumber(0.329));
    cJSON_AddItemToObject(a, "xy", xy);
    cJSON_AddStringToObject(a, "colormode", "xy");
    return a;
}

cJSON* make_groups_json(const std::map<std::string, Light>& lights,
                         const std::vector<std::string>& light_ids,
                         const std::map<std::string, Group>& groups) {
    cJSON* gj = cJSON_CreateObject();
    (void)lights;
    {
        cJSON* g0 = cJSON_CreateObject();
        cJSON_AddStringToObject(g0, "name", "Group 0");
        cJSON* g0lights = cJSON_CreateArray();
        for (auto& id : light_ids) cJSON_AddItemToArray(g0lights, cJSON_CreateString(id.c_str()));
        cJSON_AddItemToObject(g0, "lights", g0lights);
        cJSON_AddItemToObject(g0, "sensors", cJSON_CreateArray());
        cJSON_AddStringToObject(g0, "type", "LightGroup");
        cJSON* g0state = cJSON_CreateObject();
        cJSON_AddBoolToObject(g0state, "all_on", true);
        cJSON_AddBoolToObject(g0state, "any_on", true);
        cJSON_AddItemToObject(g0, "state", g0state);
        cJSON_AddBoolToObject(g0, "recycle", false);
        cJSON_AddItemToObject(g0, "action", make_group_action());
        cJSON_AddItemToObject(gj, "0", g0);
    }
    for (auto& [gid, g] : groups) {
        cJSON* gobj = cJSON_CreateObject();
        cJSON_AddStringToObject(gobj, "name", g.name.c_str());
        cJSON* glights = cJSON_CreateArray();
        for (auto& id : g.lights) cJSON_AddItemToArray(glights, cJSON_CreateString(id.c_str()));
        cJSON_AddItemToObject(gobj, "lights", glights);
        cJSON_AddItemToObject(gobj, "sensors", cJSON_CreateArray());
        cJSON_AddStringToObject(gobj, "type", g.type.c_str());
        // Old bridge: d.get("class", "Other") - always present.
        cJSON_AddStringToObject(gobj, "class", g.klass.empty() ? "Other" : g.klass.c_str());
        cJSON* gstate = cJSON_CreateObject();
        cJSON_AddBoolToObject(gstate, "all_on", true);
        cJSON_AddBoolToObject(gstate, "any_on", true);
        cJSON_AddItemToObject(gobj, "state", gstate);
        cJSON_AddBoolToObject(gobj, "recycle", false);
        cJSON_AddItemToObject(gobj, "action", make_group_action());
        if (g.type == "Entertainment") {
            cJSON* stream = cJSON_CreateObject();
            cJSON_AddStringToObject(stream, "proxymode", "auto");
            cJSON_AddStringToObject(stream, "proxynode", "/bridge");
            cJSON_AddBoolToObject(stream, "active", g.stream_active);
            if (g.stream_owner.empty()) cJSON_AddNullToObject(stream, "owner");
            else cJSON_AddStringToObject(stream, "owner", g.stream_owner.c_str());
            cJSON_AddItemToObject(gobj, "stream", stream);
            cJSON* locs = cJSON_CreateObject();
            for (auto& [lid, pos] : g.locations) {
                cJSON* arr = cJSON_CreateArray();
                cJSON_AddItemToArray(arr, cJSON_CreateNumber(pos[0]));
                cJSON_AddItemToArray(arr, cJSON_CreateNumber(pos[1]));
                cJSON_AddItemToArray(arr, cJSON_CreateNumber(pos[2]));
                cJSON_AddItemToObject(locs, lid.c_str(), arr);
            }
            cJSON_AddItemToObject(gobj, "locations", locs);
        }
        cJSON_AddItemToObject(gj, gid.c_str(), gobj);
    }
    return gj;
}

cJSON* apply_light_state_and_ack(Light& l, const std::string& addr_prefix, cJSON* body) {
    cJSON* arr = cJSON_CreateArray();
    if (!body) return arr;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, body) {
        if (!item->string) continue;
        std::string key = item->string;
        if (key == "on" && cJSON_IsBool(item)) {
            l.on = cJSON_IsTrue(item);
        } else if (key == "bri" && cJSON_IsNumber(item)) {
            l.bri = item->valueint;
        } else if (key == "xy" && cJSON_IsArray(item) && cJSON_GetArraySize(item) == 2) {
            l.x = cJSON_GetArrayItem(item, 0)->valuedouble;
            l.y = cJSON_GetArrayItem(item, 1)->valuedouble;
        } else {
            continue;  // unrecognised key: no ack entry, same as fakebridge.py only acking known keys implicitly
        }
        cJSON* entry = cJSON_CreateObject();
        cJSON* succ = cJSON_CreateObject();
        std::string addr = addr_prefix + key;
        cJSON_AddItemToObject(succ, addr.c_str(), cJSON_Duplicate(item, 1));
        cJSON_AddItemToObject(entry, "success", succ);
        cJSON_AddItemToArray(arr, entry);
    }
    return arr;
}

// "/api/<32 hex>/lights/1" -> "/api/<8 hex>.../lights/1"
std::string path_for_log(const std::string& path) {
    if (path.rfind("/api/", 0) != 0) return path;
    const size_t end = path.find('/', 5);
    const size_t len = (end == std::string::npos ? path.size() : end) - 5;
    if (len <= 12) return path;  // "config", "nouser": not a username
    return path.substr(0, 5 + 8) + "..." + (end == std::string::npos ? std::string() : path.substr(end));
}

}  // namespace

struct Bridge::Impl {
    Options opts;
    const bridge_identity::Identity& identity;
    Pairing& pairing;
    std::map<std::string, Light> lights;
    std::vector<std::string> light_ids;
    std::map<std::string, Group> groups;
    int high_water = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> lost_pairing_logged;
    const std::string groups_path;  // unused; keeps the moved code unchanged
    Response resp;

    Impl(Options o, const bridge_identity::Identity& id, Pairing& p) : opts(std::move(o)), identity(id), pairing(p) { init(); }

    struct Request {
        std::string method, path, body, user_agent;
    };

    void save_groups(const std::string&, const std::map<std::string, Group>& g, int hw) {
        if (opts.save_groups_text) opts.save_groups_text(groups_to_text(g, hw));
    }
    void send_response(int, int code, const std::string& body, const std::string& content_type) {
        resp.status = code;
        resp.body = body;
        resp.content_type = content_type;
    }
    void send_json(int, int code, cJSON* obj) {
        char* text = cJSON_PrintUnformatted(obj);
        std::string body = text ? text : "{}";
        if (text) cJSON_free(text);
        cJSON_Delete(obj);
        send_response(0, code, body, "application/json");
    }
    void close(int) {}

    void init();
    void handle(const Request& req, const std::string& src);
};

void Bridge::Impl::init() {
    // ---- initial LIGHTS/GROUPS, same shape fakebridge.py starts with ------
    {
        int i = 1;
        for (const auto& name : opts.lamp_names) {
            std::string id = std::to_string(i);
            lights[id] = Light{name, true, 254, 0.3127, 0.329};
            light_ids.push_back(id);
            ++i;
        }
    }
    high_water = 0;
    const std::string stored_text = opts.load_groups_text ? opts.load_groups_text() : std::string();
    // v0.32: a store this version wrote ("_high_water" is in it) is left as it
    // is: a hole in it is a deleted group and its neighbours keep their ids.
    // Only a store from v0.30 or older is tidied (v0.31 tidied on every start,
    // which renumbered the groups after a DELETE + restart).
    const bool own_format = stored_text.find("\"_high_water\"") != std::string::npos;
    groups = groups_from_text(stored_text, &high_water);
    const size_t stored = groups.size();
    if (groups.empty()) {
        // Fresh start: one Entertainment group with every lamp, at id 1 (the
        // first id the TV asks when it has nothing remembered).
        Group g;
        g.name = "TV";
        g.lights = light_ids;
        g.type = "Entertainment";
        g.klass = "TV";
        g.locations = bottom_locations(light_ids);
        groups["1"] = g;
    }
    {
        const int hw_before = high_water;
        if (own_format) {
            for (const auto& [gid, g] : groups) {
                (void)g;
                if (is_group_number(gid)) high_water = std::max(high_water, std::stoi(gid));
            }
        } else if (normalize_groups(groups, high_water)) {
            AMBI_LOG("[rest] groups store tidied: %zu stored group(s) -> %zu (one per type+lights, ids "
                                 "1..%zu); ids up to %d that are gone answer as empty placeholders, not errors\n",
                         stored, groups.size(), groups.size(), high_water);
            save_groups(groups_path, groups, high_water);
        } else if (high_water != hw_before || stored == 0) {
            save_groups(groups_path, groups, high_water);
        }
    }
    AMBI_LOG("[rest] groups store: %zu group(s), highest id ever %d (%s)\n", groups.size(), high_water,
             opts.save_groups_text ? "persistent" : "persistence disabled");

}

void Bridge::Impl::handle(const Request& req, const std::string& src) {
    const int fd = 0;  // no socket here; send_json()/send_response() fill resp
    {
        {
            std::string rg, ro;
            if (opts.take_resumed_stream && opts.take_resumed_stream(rg, ro)) {
                auto it = groups.find(rg);
                if (it != groups.end()) {
                    it->second.stream_active = true;
                    it->second.stream_owner = ro;
                    AMBI_LOG("[rest] group %s stream.active=true owner=%.8s... restored (the TV's DTLS "
                                         "session continued after a restart)\n", rg.c_str(), ro.c_str());
                }
            }
        }

        std::string path = req.path;
        size_t qpos = path.find('?');
        if (qpos != std::string::npos) path = path.substr(0, qpos);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        if (path.empty()) path = "/";

        // (GET /description.xml gets its own line with the User-Agent below.)
        if ((req.method != "PUT" || path.find("/lights/") == std::string::npos) &&
            !(req.method == "GET" && path == "/description.xml")) {
            // v0.32: the username in the path is cut to 8 characters (until
            // v0.31 this line wrote it in full; it is the TV's login to the
            // bridge and the DTLS identity, and logs get shared).
            AMBI_LOG("[rest] %s %s %s (%zu-byte body)\n", src.c_str(), req.method.c_str(),
                         path_for_log(path).c_str(), req.body.size());
        }

        cJSON* body = nullptr;
        if (!req.body.empty()) body = cJSON_Parse(req.body.c_str());

        // ---- fixed routes -------------------------------------------------
        if (req.method == "GET" && path == "/description.xml") {
            // v0.17: say WHO asked. Browsers (Chrome/Edge cast discovery),
            // Electron apps, Home Assistant, the Hue app... all fetch this
            // after an SSDP answer, and their User-Agent names them.
            std::string ua = req.user_agent.empty() ? "(none)" : req.user_agent.substr(0, 200);
            for (char& c : ua)
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
            AMBI_LOG("[rest] %s description.xml asked by User-Agent: %s\n", src.c_str(), ua.c_str());
            if (opts.on_description) opts.on_description(src, ua);
            send_response(fd, 200, bridge_identity::description_xml(identity), "text/xml");
            if (body) cJSON_Delete(body);
            close(fd);
            return;
        }

        if (req.method == "POST" && path == "/api") {
            // v0.24: another IP than the TV's may pair only while the
            // link button is open (until v0.23: never, and silently).
            if (!opts.tv_ip.empty() && src != opts.tv_ip && pairing.link_remaining_ms() <= 0) {
                AMBI_LOG(
                             "[rest] %s pairing refused: not the TV (%s) and the link button is not pressed\n",
                             src.c_str(), opts.tv_ip.c_str());
                send_json(fd, 200, make_error("", 101, "link button not pressed"));
                if (body) cJSON_Delete(body);
                close(fd);
                return;
            }
            if (opts.require_link_button && pairing.link_remaining_ms() <= 0) {
                AMBI_LOG(
                             "[rest] %s pairing refused: link button not pressed "
                             "(press Pair on the monitor page or 'p' in the terminal)\n",
                             src.c_str());
                pairing.note_pair_refused();
                send_json(fd, 200, make_error("", 101, "link button not pressed"));
                if (body) cJSON_Delete(body);
                close(fd);
                return;
            }
            std::string devicetype = "unknown";
            bool generate_clientkey = false;
            if (body) {
                cJSON* dt = cJSON_GetObjectItemCaseSensitive(body, "devicetype");
                if (cJSON_IsString(dt) && dt->valuestring) devicetype = dt->valuestring;
                cJSON* gck = cJSON_GetObjectItemCaseSensitive(body, "generateclientkey");
                if (cJSON_IsBool(gck)) generate_clientkey = cJSON_IsTrue(gck);
            }
            auto [username, user] = pairing.pair(devicetype, generate_clientkey);
            cJSON* ok = cJSON_CreateObject();
            cJSON_AddStringToObject(ok, "username", username.c_str());
            if (!user.clientkey_hex.empty()) {
                std::string upper = user.clientkey_hex;
                std::transform(upper.begin(), upper.end(), upper.begin(),
                                [](unsigned char c) { return std::toupper(c); });
                cJSON_AddStringToObject(ok, "clientkey", upper.c_str());
            }
            AMBI_LOG("[rest] pairing ok devicetype=\"%s\" username=%.8s...\n",
                         devicetype.c_str(), username.c_str());
            cJSON* arr = cJSON_CreateArray();
            cJSON* wrap = cJSON_CreateObject();
            cJSON_AddItemToObject(wrap, "success", ok);
            cJSON_AddItemToArray(arr, wrap);
            send_json(fd, 200, arr);
            if (body) cJSON_Delete(body);
            close(fd);
            return;
        }

        if (req.method == "GET" && (path == "/api/config" || path == "/config" || path == "/api/nouser/config")) {
            cJSON* c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "name", identity.name.c_str());
            cJSON_AddStringToObject(c, "datastoreversion", "131");
            cJSON_AddStringToObject(c, "swversion", opts.swversion.c_str());
            cJSON_AddStringToObject(c, "apiversion", opts.apiversion.c_str());
            cJSON_AddStringToObject(c, "mac", identity.mac.c_str());
            cJSON_AddStringToObject(c, "bridgeid", identity.bridgeid.c_str());
            cJSON_AddBoolToObject(c, "factorynew", false);
            cJSON_AddNullToObject(c, "replacesbridgeid");
            cJSON_AddStringToObject(c, "modelid", "BSB002");
            cJSON_AddStringToObject(c, "starterkitid", "");
            send_json(fd, 200, c);
            if (body) cJSON_Delete(body);
            close(fd);
            return;
        }

        // ---- /api/<user>/... ------------------------------------------------
        if (path.rfind("/api/", 0) == 0) {
            std::string rest_path = path.substr(5);  // after "/api/"
            size_t slash = rest_path.find('/');
            std::string user = slash == std::string::npos ? rest_path : rest_path.substr(0, slash);
            std::string sub = slash == std::string::npos ? "" : rest_path.substr(slash);

            // v0.24 (PLAN item 2): an unknown username gets what a real Hue
            // bridge gives: GET .../config -> the short public config (no
            // whitelist), everything else -> error 1 "unauthorized user".
            // Until v0.23 any username got the full answer. The TV starts
            // with GET /api/<user>/config, so that path keeps answering.
            if (!pairing.get(user)) {
                // v0.31: a 32-hex-digit name we never issued is the TV's own
                // fallback (TVInfo: MD5 of its production code) - it uses it
                // when it has no stored username for this bridge any more,
                // i.e. it LOST the pairing and will not stream until paired
                // again. Said once a minute per name, not on every request.
                if (user.size() == 32 && std::all_of(user.begin(), user.end(), [](unsigned char c) {
                        return std::isxdigit(c) != 0; })) {
                    const auto now = std::chrono::steady_clock::now();
                    auto seen = lost_pairing_logged.find(user);
                    if (seen == lost_pairing_logged.end() || now - seen->second > std::chrono::seconds(60)) {
                        if (lost_pairing_logged.size() > 64) lost_pairing_logged.clear();
                        lost_pairing_logged[user] = now;
                        AMBI_LOG(
                                     "[rest] %s uses an UNKNOWN 32-hex username (%.8s...): the TV has LOST ITS PAIRING "
                                     "with this bridge (this name is its built-in fallback, not one we issued). It "
                                     "cannot stream until it pairs again: run Configure in the TV's Ambilight+hue "
                                     "menu and press Pair here.\n", src.c_str(), user.c_str());
                        pairing.note_pair_refused();
                    }
                }
                AMBI_LOG("[rest] unknown user %.12s%s from %s -> %s\n", user.c_str(),
                             user.size() > 12 ? "..." : "", src.c_str(),
                             req.method == "GET" && sub == "/config" ? "short config" : "error 1");
                if (req.method == "GET" && sub == "/config") {
                    cJSON* c = cJSON_CreateObject();
                    cJSON_AddStringToObject(c, "name", identity.name.c_str());
                    cJSON_AddStringToObject(c, "datastoreversion", "131");
                    cJSON_AddStringToObject(c, "swversion", opts.swversion.c_str());
                    cJSON_AddStringToObject(c, "apiversion", opts.apiversion.c_str());
                    cJSON_AddStringToObject(c, "mac", identity.mac.c_str());
                    cJSON_AddStringToObject(c, "bridgeid", identity.bridgeid.c_str());
                    cJSON_AddBoolToObject(c, "factorynew", false);
                    cJSON_AddNullToObject(c, "replacesbridgeid");
                    cJSON_AddStringToObject(c, "modelid", "BSB002");
                    cJSON_AddStringToObject(c, "starterkitid", "");
                    send_json(fd, 200, c);
                } else {
                    send_json(fd, 200, make_error(sub.empty() ? "/" : sub, 1, "unauthorized user"));
                }
                if (body) cJSON_Delete(body);
                close(fd);
                return;
            }

            if (req.method == "GET") {
                if (sub.empty() || sub == "/config") {
                    cJSON* c = cJSON_CreateObject();
                    cJSON_AddStringToObject(c, "name", identity.name.c_str());
                    cJSON_AddStringToObject(c, "datastoreversion", "131");
                    cJSON_AddStringToObject(c, "swversion", opts.swversion.c_str());
                    cJSON_AddStringToObject(c, "apiversion", opts.apiversion.c_str());
                    cJSON_AddStringToObject(c, "mac", identity.mac.c_str());
                    cJSON_AddStringToObject(c, "bridgeid", identity.bridgeid.c_str());
                    cJSON_AddBoolToObject(c, "factorynew", false);
                    cJSON_AddNullToObject(c, "replacesbridgeid");
                    cJSON_AddStringToObject(c, "modelid", "BSB002");
                    cJSON_AddStringToObject(c, "starterkitid", "");
                    // Full config, field for field and in the same order as
                    // the old Python bridge's Ident.full_config(). Until v8
                    // the C++ version left out "whitelist" (the list of
                    // paired usernames every real bridge returns) and the
                    // portal/proxy/swupdate/backup blocks. Found by replaying
                    // the TV's exact requests against both bridges and
                    // diffing the replies; the TV, after a power cycle,
                    // re-requested pairing (POST /api) and greyed out
                    // "Immersive experience" against the C++ bridge.
                    {
                        std::string ts = now_iso();
                        cJSON* wl = cJSON_CreateObject();
                        for (auto& [uname, devtype] : pairing.users_with_devicetype()) {
                            cJSON* e = cJSON_CreateObject();
                            cJSON_AddStringToObject(e, "last use date", ts.c_str());
                            cJSON_AddStringToObject(e, "create date", ts.c_str());
                            cJSON_AddStringToObject(e, "name", devtype.c_str());
                            cJSON_AddItemToObject(wl, uname.c_str(), e);
                        }
                        cJSON_AddItemToObject(c, "whitelist", wl);
                    }
                    cJSON_AddStringToObject(c, "UTC", now_iso().c_str());
                    cJSON_AddStringToObject(c, "localtime", now_iso().c_str());
                    cJSON_AddStringToObject(c, "timezone", "Europe/Athens");
                    cJSON_AddBoolToObject(c, "linkbutton", false);
                    cJSON_AddNumberToObject(c, "zigbeechannel", 25);
                    cJSON_AddStringToObject(c, "ipaddress", identity.ip.c_str());
                    cJSON_AddStringToObject(c, "netmask", "255.255.255.0");
                    {
                        std::string gw = identity.ip;
                        size_t dot = gw.rfind('.');
                        gw = (dot == std::string::npos) ? gw : gw.substr(0, dot) + ".1";
                        cJSON_AddStringToObject(c, "gateway", gw.c_str());
                    }
                    cJSON_AddBoolToObject(c, "dhcp", true);
                    cJSON_AddStringToObject(c, "proxyaddress", "none");
                    cJSON_AddNumberToObject(c, "proxyport", 0);
                    cJSON_AddBoolToObject(c, "portalservices", false);
                    cJSON_AddStringToObject(c, "portalconnection", "disconnected");
                    {
                        cJSON* ps = cJSON_CreateObject();
                        cJSON_AddBoolToObject(ps, "signedon", false);
                        cJSON_AddBoolToObject(ps, "incoming", false);
                        cJSON_AddBoolToObject(ps, "outgoing", false);
                        cJSON_AddStringToObject(ps, "communication", "disconnected");
                        cJSON_AddItemToObject(c, "portalstate", ps);
                        cJSON* is = cJSON_CreateObject();
                        cJSON_AddStringToObject(is, "internet", "disconnected");
                        cJSON_AddStringToObject(is, "remoteaccess", "disconnected");
                        cJSON_AddStringToObject(is, "time", "disconnected");
                        cJSON_AddStringToObject(is, "swupdate", "disconnected");
                        cJSON_AddItemToObject(c, "internetservices", is);
                        std::string ts = now_iso();
                        cJSON* su = cJSON_CreateObject();
                        cJSON_AddBoolToObject(su, "checkforupdate", false);
                        cJSON_AddStringToObject(su, "lastchange", ts.c_str());
                        cJSON_AddStringToObject(su, "state", "noupdates");
                        cJSON* sb = cJSON_CreateObject();
                        cJSON_AddStringToObject(sb, "state", "noupdates");
                        cJSON_AddStringToObject(sb, "lastinstall", ts.c_str());
                        cJSON_AddItemToObject(su, "bridge", sb);
                        cJSON* ai = cJSON_CreateObject();
                        cJSON_AddStringToObject(ai, "updatetime", "T14:00:00");
                        cJSON_AddBoolToObject(ai, "on", false);
                        cJSON_AddItemToObject(su, "autoinstall", ai);
                        cJSON_AddItemToObject(c, "swupdate2", su);
                        cJSON* bk = cJSON_CreateObject();
                        cJSON_AddStringToObject(bk, "status", "idle");
                        cJSON_AddNumberToObject(bk, "errorcode", 0);
                        cJSON_AddItemToObject(c, "backup", bk);
                    }

                    if (sub.empty()) {
                        cJSON* root = cJSON_CreateObject();
                        cJSON* lj = cJSON_CreateObject();
                        for (auto& [id, l] : lights) {
                            cJSON_AddItemToObject(lj, id.c_str(),
                                                   make_light_json(std::stoi(id), l.name, identity.serial, l));
                        }
                        cJSON_AddItemToObject(root, "lights", lj);
                        cJSON_AddItemToObject(root, "groups", make_groups_json(lights, light_ids, groups));
                        cJSON_AddItemToObject(root, "config", c);
                        cJSON_AddItemToObject(root, "schedules", cJSON_CreateObject());
                        cJSON_AddItemToObject(root, "scenes", cJSON_CreateObject());
                        cJSON_AddItemToObject(root, "rules", cJSON_CreateObject());
                        cJSON_AddItemToObject(root, "sensors", cJSON_CreateObject());
                        cJSON_AddItemToObject(root, "resourcelinks", cJSON_CreateObject());
                        send_json(fd, 200, root);
                    } else {
                        send_json(fd, 200, c);
                    }
                } else if (sub == "/lights") {
                    cJSON* lj = cJSON_CreateObject();
                    for (auto& [id, l] : lights) {
                        cJSON_AddItemToObject(lj, id.c_str(), make_light_json(std::stoi(id), l.name, identity.serial, l));
                    }
                    send_json(fd, 200, lj);
                } else if (sub.rfind("/lights/", 0) == 0 && opts.fail_lamp_check && opts.fail_lamp_check(src)) {
                    // v0.37: see Options::fail_lamp_check - a failing check makes the TV reconnect by itself
                    send_response(fd, 503, "{\"error\":\"old DTLS session - reconnect\"}", "application/json");
                } else if (sub.rfind("/lights/", 0) == 0) {
                    std::string lid = sub.substr(8);
                    auto it = lights.find(lid);
                    if (it != lights.end()) {
                        send_json(fd, 200, make_light_json(std::stoi(lid), it->second.name, identity.serial, it->second));
                    } else {
                        send_json(fd, 200, make_error(sub, 3, "resource not available"));
                    }
                } else if (sub == "/groups") {
                    send_json(fd, 200, make_groups_json(lights, light_ids, groups));
                } else if (sub.rfind("/groups/", 0) == 0) {
                    std::string gid = sub.substr(8);
                    if (gid == "0") {
                        cJSON* all = make_groups_json(lights, light_ids, groups);
                        cJSON* g0 = cJSON_DetachItemFromObjectCaseSensitive(all, "0");
                        cJSON_Delete(all);
                        send_json(fd, 200, g0 ? g0 : cJSON_CreateObject());
                    } else if (groups.count(gid)) {
                        {
                            int ent = 0;
                            for (auto& [id2, g2] : groups) ent += g2.type == "Entertainment" ? 1 : 0;
                            const Group& gg = groups.at(gid);
                            AMBI_LOG("[rest] %s asked groups/%s -> answered %s '%s' (created %s; %zu groups stored, %d Entertainment)\n",
                                         src.c_str(), gid.c_str(), gg.type.c_str(), gg.name.c_str(),
                                         gg.created.empty() ? "unknown" : gg.created.c_str(), groups.size(), ent);
                        }
                        if (groups.at(gid).type != "Entertainment" && opts.on_lightgroup_used) opts.on_lightgroup_used();
                        cJSON* all = make_groups_json(lights, light_ids, groups);
                        cJSON* g = cJSON_DetachItemFromObjectCaseSensitive(all, gid.c_str());
                        cJSON_Delete(all);
                        send_json(fd, 200, g ? g : cJSON_CreateObject());
                    } else if (is_group_number(gid) && std::stoi(gid) <= high_water) {
                        // v0.31: an id that existed once. No "error" here: the TV
                        // gives up a whole session when the id it remembers
                        // answers with an error, and stops its 1,2,3.. search at
                        // the first error. An empty LightGroup never matches its
                        // lamps, so it simply looks further.
                        AMBI_LOG("[rest] %s asked groups/%s -> gone (answered an empty placeholder; "
                                             "%zu groups stored, highest id ever %d)\n",
                                     src.c_str(), gid.c_str(), groups.size(), high_water);
                        cJSON* ph = cJSON_CreateObject();
                        cJSON_AddStringToObject(ph, "name", "(removed)");
                        cJSON_AddItemToObject(ph, "lights", cJSON_CreateArray());
                        cJSON_AddItemToObject(ph, "sensors", cJSON_CreateArray());
                        cJSON_AddStringToObject(ph, "type", "LightGroup");
                        cJSON_AddBoolToObject(ph, "recycle", false);
                        send_json(fd, 200, ph);
                    } else {
                        // the TV's HueControl.CheckHueGroup stops scanning group ids
                        // on an "error" reply - mirrored exactly from fakebridge.py.
                        AMBI_LOG("[rest] %s asked groups/%s -> answered NOT FOUND (error 3; %zu groups stored)\n",
                                     src.c_str(), gid.c_str(), groups.size());
                        send_json(fd, 200, make_error(sub, 3, "resource, " + sub + ", not available"));
                    }
                } else if (sub == "/capabilities") {
                    cJSON* c = cJSON_CreateObject();
                    auto avail_total = [&](const char* k, int a, int t) {
                        cJSON* o = cJSON_CreateObject();
                        cJSON_AddNumberToObject(o, "available", a);
                        cJSON_AddNumberToObject(o, "total", t);
                        cJSON_AddItemToObject(c, k, o);
                    };
                    avail_total("lights", 60, 63);
                    avail_total("sensors", 240, 250);
                    avail_total("groups", 60, 64);
                    avail_total("scenes", 172, 200);
                    avail_total("rules", 233, 250);
                    avail_total("schedules", 95, 100);
                    avail_total("resourcelinks", 59, 64);
                    cJSON* streaming = cJSON_CreateObject();
                    cJSON_AddNumberToObject(streaming, "available", 1);
                    cJSON_AddNumberToObject(streaming, "total", 1);
                    cJSON_AddNumberToObject(streaming, "channels", 10);
                    cJSON_AddItemToObject(c, "streaming", streaming);
                    cJSON* tz = cJSON_CreateObject();
                    cJSON* tzv = cJSON_CreateArray();
                    cJSON_AddItemToArray(tzv, cJSON_CreateString("Europe/Athens"));
                    cJSON_AddItemToObject(tz, "values", tzv);
                    cJSON_AddItemToObject(c, "timezones", tz);
                    send_json(fd, 200, c);
                } else {
                    send_json(fd, 200, cJSON_CreateObject());
                }
            } else if (req.method == "PUT") {
                if (sub.rfind("/lights/", 0) == 0 && sub.find("/state") != std::string::npos) {
                    std::string lid = sub.substr(8, sub.find("/state") - 8);
                    auto it = lights.find(lid);
                    if (it != lights.end()) {
                        cJSON* acks = apply_light_state_and_ack(it->second, "/lights/" + lid + "/state/", body);
                        if (cJSON_GetArraySize(acks) == 0) {
                            cJSON_Delete(acks);
                            send_json(fd, 200, wrap_success(cJSON_CreateObject()));
                        } else {
                            send_json(fd, 200, acks);
                        }
                    } else {
                        send_json(fd, 200, make_error(sub, 3, "resource not available"));
                    }
                } else if (sub.rfind("/groups/", 0) == 0) {
                    std::string rem = sub.substr(8);
                    size_t slash2 = rem.find('/');
                    std::string gid = slash2 == std::string::npos ? rem : rem.substr(0, slash2);
                    std::string gsub = slash2 == std::string::npos ? "" : rem.substr(slash2);

                    if (gsub == "/action" && body) {
                        auto it = groups.find(gid);
                        if (it != groups.end()) {
                            for (auto& lid2 : it->second.lights) {
                                auto lit = lights.find(lid2);
                                if (lit != lights.end()) {
                                    cJSON* throwaway = apply_light_state_and_ack(lit->second, "", body);
                                    cJSON_Delete(throwaway);
                                }
                            }
                        }
                        cJSON* o = cJSON_CreateObject();
                        cJSON_AddStringToObject(o, ("/groups/" + gid + "/action").c_str(), "ok");
                        send_json(fd, 200, wrap_success(o));
                    } else if (gsub.empty() && body != nullptr && cJSON_IsObject(body) &&
                               cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(body, "stream"))) {
                        cJSON* stream_obj = cJSON_GetObjectItemCaseSensitive(body, "stream");
                        cJSON* active_item = cJSON_GetObjectItemCaseSensitive(stream_obj, "active");
                        bool active = cJSON_IsBool(active_item) && cJSON_IsTrue(active_item);
                        auto it = groups.find(gid);
                        if (it != groups.end()) {
                            it->second.stream_active = active;
                            it->second.stream_owner = active ? user : "";
                            if (opts.on_stream_active) opts.on_stream_active(user, gid, active, it->second.lights);
                            AMBI_LOG("[rest] group %s stream.active=%s owner=%.8s...\n",
                                         gid.c_str(), active ? "true" : "false", user.c_str());
                        }
                        cJSON* o = cJSON_CreateObject();
                        cJSON_AddBoolToObject(o, ("/groups/" + gid + "/stream/active").c_str(), active);
                        send_json(fd, 200, wrap_success(o));
                    } else if (gsub.empty() && body && cJSON_HasObjectItem(body, "lights") && !groups.count(gid)) {
                        // v0.31: until v0.30 this silently created group <gid> (any id).
                        send_json(fd, 200, make_error(sub, 3, "resource, " + sub + ", not available"));
                    } else if (gsub.empty() && body && cJSON_HasObjectItem(body, "lights")) {
                        auto& g = groups[gid];
                        cJSON* lj2 = cJSON_GetObjectItemCaseSensitive(body, "lights");
                        g.lights.clear();
                        cJSON* item = nullptr;
                        cJSON_ArrayForEach(item, lj2) {
                            if (cJSON_IsString(item)) g.lights.push_back(item->valuestring);
                        }
                        if (g.type == "Entertainment") g.locations = bottom_locations(g.lights);
                        save_groups(groups_path, groups, high_water);
                        cJSON* o = cJSON_CreateObject();
                        cJSON_AddStringToObject(o, ("/groups/" + gid).c_str(), "ok");
                        send_json(fd, 200, wrap_success(o));
                    } else {
                        cJSON* o = cJSON_CreateObject();
                        cJSON_AddStringToObject(o, sub.c_str(), "ok");
                        send_json(fd, 200, wrap_success(o));
                    }
                } else {
                    cJSON* o = cJSON_CreateObject();
                    cJSON_AddStringToObject(o, sub.c_str(), "ok");
                    send_json(fd, 200, wrap_success(o));
                }
            } else if (req.method == "POST" && sub == "/groups") {
                Group g;
                g.name.clear();  // default decided below, after the type is known
                if (body) {
                    cJSON* type_item = cJSON_GetObjectItemCaseSensitive(body, "type");
                    if (cJSON_IsString(type_item)) g.type = type_item->valuestring;
                    cJSON* name_item = cJSON_GetObjectItemCaseSensitive(body, "name");
                    // Same defaults as the old bridge: an Entertainment group
                    // the TV creates without a name is "TV" (_new_group's
                    // setdefault), any other group is "Group <id>" (_groups()).
                    g.name = (cJSON_IsString(name_item) && name_item->valuestring) ? name_item->valuestring : "";

                    cJSON* class_item = cJSON_GetObjectItemCaseSensitive(body, "class");
                    if (cJSON_IsString(class_item)) g.klass = class_item->valuestring;
                    cJSON* lights_item = cJSON_GetObjectItemCaseSensitive(body, "lights");
                    cJSON* item = nullptr;
                    cJSON_ArrayForEach(item, lights_item) {
                        if (cJSON_IsString(item)) g.lights.push_back(item->valuestring);
                    }
                }
                if (g.type != "Entertainment" && opts.on_lightgroup_used) opts.on_lightgroup_used();
                // v0.31: the same type + lights again = the group we already
                // have (until v0.30 every such POST made one more group).
                std::string gid;
                for (const auto& [eid, eg] : groups) {
                    if (group_key(eg) == group_key(g)) {
                        gid = eid;
                        break;
                    }
                }
                if (!gid.empty()) {
                    AMBI_LOG("[rest] POST /groups type=%s lights=%zu -> group %s already has them, reused "
                                         "(%zu groups stored)\n", g.type.c_str(), g.lights.size(), gid.c_str(),
                                 groups.size());
                } else {
                    gid = lowest_free_group_id(groups);
                    if (g.name.empty()) g.name = (g.type == "Entertainment") ? "TV" : "Group " + gid;
                    if (g.type == "Entertainment") g.locations = bottom_locations(g.lights);
                    {
                        std::time_t tt = std::time(nullptr);
                        std::tm tm{};
                        localtime_r(&tt, &tm);
                        char ts[32];
                        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
                        g.created = ts;
                    }
                    groups[gid] = g;
                    high_water = std::max(high_water, std::stoi(gid));
                    save_groups(groups_path, groups, high_water);
                    AMBI_LOG("[rest] created group %s type=%s lights=%zu\n",
                                 gid.c_str(), g.type.c_str(), g.lights.size());
                }
                cJSON* id_obj = cJSON_CreateString(gid.c_str());
                send_json(fd, 200, make_success("id", id_obj));
            } else if (req.method == "POST") {
                send_json(fd, 200, make_success("id", cJSON_CreateString("1")));
            } else if (req.method == "DELETE" && sub.rfind("/groups/", 0) == 0) {
                // v0.31: really removes the group (until v0.30: "deleted" and
                // nothing happened). Its id stays known (high_water), so a
                // later GET answers the empty placeholder, not an error.
                const std::string gid = sub.substr(8);
                auto it = groups.find(gid);
                if (it == groups.end()) {
                    send_json(fd, 200, make_error(sub, 3, "resource, " + sub + ", not available"));
                } else {
                    if (it->second.stream_active && opts.on_stream_active) {
                        opts.on_stream_active(it->second.stream_owner, gid, false, it->second.lights);
                    }
                    AMBI_LOG("[rest] %s deleted group %s (%s, %zu lights); %zu left\n", src.c_str(),
                                 gid.c_str(), it->second.type.c_str(), it->second.lights.size(), groups.size() - 1);
                    groups.erase(it);
                    save_groups(groups_path, groups, high_water);
                    send_json(fd, 200, wrap_success(cJSON_CreateString((path + " deleted").c_str())));
                }
            } else if (req.method == "DELETE") {
                send_json(fd, 200, wrap_success(cJSON_CreateString((path + " deleted").c_str())));
            } else {
                send_json(fd, 200, cJSON_CreateObject());
            }
            if (body) cJSON_Delete(body);
            close(fd);
            return;
        }

        send_json(fd, 404, make_error(path, 4, "method not available"));
        if (body) cJSON_Delete(body);
    }
}

Bridge::Bridge(Options options, const bridge_identity::Identity& identity, Pairing& pairing)
    : impl_(new Impl(std::move(options), identity, pairing)) {}

Bridge::~Bridge() { delete impl_; }

Response Bridge::handle(const std::string& method, const std::string& path, const std::string& body,
                        const std::string& src_ip, const std::string& user_agent) {
    impl_->resp = Response();
    impl_->handle(Impl::Request{method, path, body, user_agent}, src_ip);
    return impl_->resp;
}

}  // namespace hue_core
