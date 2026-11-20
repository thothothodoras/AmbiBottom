// v0.17: the bridge's identities (MAC + name), chosen on the monitor page.
//
// <state-dir>/identity.json = {"active": "<mac>", "identities": [{"mac", "name"}, ...]}.
// Missing or unreadable -> one identity, the old fixed default
// (00:17:88:a4:5e:01 / FakeBridge-A45E01), so an existing install behaves
// exactly as before.
//
// Each identity has its own pairing.json + groups.json, because the TV
// pairs with a MAC: the default identity keeps using <state-dir>/ directly
// (the files that already exist there - nothing moves), every other one
// uses <state-dir>/identities/<mac without colons>/.
//
// The lamp count (3 or 5) is part of the identity too (v0.17; before, only
// --lamps): the TV configures its Entertainment group per bridge.
//
// The running bridge never switches identity on the fly (SSDP, mDNS, REST
// and DTLS all carry it): a change is saved and applies at the next start.
#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace identity_store {

struct Entry {
    std::string mac;   // lowercase "xx:xx:xx:xx:xx:xx"
    std::string name;
    int lamps = 3;     // bottom lamps handed to the TV: 3 or 5 (the TV's group is per identity)
};

struct Store {
    std::string active;          // mac of the identity to use at the next start
    std::vector<Entry> entries;  // never empty after load()
    const Entry* find(const std::string& mac) const;
};

// "xx:xx:xx:xx:xx:xx", hex, any case.
bool valid_mac(const std::string& mac);
std::string normalize_mac(const std::string& mac);  // lowercase
// Bridge name: 1-32 printable ASCII characters, none of < > & " ' (it goes
// into description.xml, which the TV parses byte by byte), no leading or
// trailing spaces. On failure `err` gets a short reason.
bool valid_name(const std::string& name, std::string* err);
// "FakeBridge-" + the last 6 hex digits of the MAC, uppercase.
std::string default_name_for(const std::string& mac);
// 00:17:88 (Philips/Signify prefix, like the default) + 3 random bytes.
std::string random_mac();

Store load(const std::string& state_dir);
bool save(const Store& s, const std::string& state_dir);
// Directory holding this identity's pairing.json / groups.json.
std::string dir_for(const std::string& state_dir, const std::string& mac);

// Thread-safe holder used by the monitor page.
class Shared {
public:
    Shared(std::string state_dir, Entry running);
    Store snapshot() const;
    const Entry& running() const { return running_; }
    const std::string& state_dir() const { return state_dir_; }
    // Each returns false with `err` set on bad input or a failed disk write.
    bool select(const std::string& mac, std::string* err);
    bool rename(const std::string& mac, const std::string& name, std::string* err);
    bool set_lamps(const std::string& mac, int lamps, std::string* err);
    // New random MAC, given name (or the default name when empty); becomes active.
    bool create(const std::string& name, std::string* new_mac, std::string* err);

private:
    mutable std::mutex mu_;
    std::string state_dir_;
    Entry running_;
    Store store_;
};

}  // namespace identity_store
