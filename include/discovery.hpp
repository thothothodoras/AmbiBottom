// v0.19: who is looking for this bridge, and the "answer only Hue
// searches" switch.
//
// Every SSDP M-SEARCH and every GET /description.xml is noted here (IP,
// what it searched for = ST, User-Agent), grouped and counted, so the
// monitor page can show the whole User-Agent (the terminal box cuts it).
// The full lines also stay in <state-dir>/ambi_bridge.log as before.
//
// The switch (saved in <state-dir>/discovery.json, default OFF = answer
// every search, as the Python bridge and every version up to v0.18 did):
// when ON, the SSDP responder answers only searches a Hue client makes
// (ssdp:all, upnp:rootdevice, urn:schemas-upnp-org:device:basic:1, our own
// uuid) - like a real Hue bridge - and ignores e.g. a browser's
// cast search (urn:dial-multiscreen-org:service:dial:1). mDNS is not
// affected.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace discovery {

// Indices into the responder's 3 variants: 0 upnp:rootdevice, 1 uuid:<udn>,
// 2 urn:schemas-upnp-org:device:basic:1. hue_only=false -> all three (old
// behaviour). hue_only=true -> ssdp:all: all three; one of ours: just that
// one; anything else: none. `st` is compared case-insensitively, trimmed.
std::vector<int> answer_variants(const std::string& st, const std::string& udn, bool hue_only);

struct Asker {
    std::string ip;
    std::string kind;  // "ssdp" or "description"
    std::string what;  // ST for ssdp, "-" for description.xml
    std::string ua;    // User-Agent ("-" when none)
    long count = 0;
    int64_t first_ms = 0, last_ms = 0;  // unix ms
    bool answered = true;               // ssdp: did we answer the last one
};

class Registry {
public:
    // Loads <state_dir>/discovery.json; an empty state_dir = not saved (tests).
    explicit Registry(std::string state_dir);
    bool hue_only() const;
    bool set_hue_only(bool on, std::string* err);
    void note(const std::string& ip, const std::string& kind, const std::string& what, const std::string& ua,
              bool answered, int64_t now_ms);
    std::vector<Asker> snapshot() const;  // most recent first
    static constexpr size_t kMaxEntries = 40;

private:
    mutable std::mutex mu_;
    std::string state_dir_;
    bool hue_only_ = false;
    std::vector<Asker> askers_;
};

int64_t now_ms();

}  // namespace discovery
