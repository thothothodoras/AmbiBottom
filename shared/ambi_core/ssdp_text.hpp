// The SSDP texts a Hue bridge sends, as plain strings (no sockets): which
// search targets to answer, the M-SEARCH reply and the NOTIFY. Same wording
// as the Linux bridge's src/mdns_ssdp.cpp (which still carries its own copy;
// the ESP32 usermod uses this header). C++ standard library only.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "bridge_identity.hpp"

namespace ssdp_text {

inline const char* group() { return "239.255.255.250"; }
constexpr int port = 1900;

// {ST / NT, USN}
inline std::vector<std::pair<std::string, std::string>> variants(const bridge_identity::Identity& id) {
    const std::string udn = "uuid:" + id.udn;
    return {{"upnp:rootdevice", udn + "::upnp:rootdevice"}, {udn, udn}, {"urn:schemas-upnp-org:device:basic:1", udn}};
}

inline std::string location(const bridge_identity::Identity& id) {
    return "http://" + id.ip + ":" + std::to_string(id.port) + "/description.xml";
}

// The ST header of an M-SEARCH (ssdp:discover) datagram; "" when it is not one.
inline std::string search_target(const std::string& msg) {
    if (msg.rfind("M-SEARCH", 0) != 0 || msg.find("ssdp:discover") == std::string::npos) return {};
    std::string lower = msg;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t p = lower.find("\r\nst:");
    if (p == std::string::npos) return {};
    p += 5;
    const size_t e = msg.find("\r\n", p);
    std::string v = msg.substr(p, e == std::string::npos ? std::string::npos : e - p);
    const size_t s = v.find_first_not_of(' ');
    return s == std::string::npos ? std::string() : v.substr(s);
}

// Which variants to answer for a search target. A Hue bridge answers
// ssdp:all, upnp:rootdevice, its own uuid and the "basic" device type - not
// DIAL / MediaServer / anything else (the Linux bridge's "answer only Hue
// searches" switch, which is the only behaviour here).
inline std::vector<int> answers(const std::string& st, const bridge_identity::Identity& id) {
    std::string l = st;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (l == "ssdp:all") return {0, 1, 2};
    if (l == "upnp:rootdevice") return {0};
    if (l == "uuid:" + id.udn) return {1};
    if (l == "urn:schemas-upnp-org:device:basic:1") return {2};
    return {};
}

inline std::string search_reply(const bridge_identity::Identity& id, int variant) {
    const auto v = variants(id)[static_cast<size_t>(variant)];
    return "HTTP/1.1 200 OK\r\nHOST: 239.255.255.250:1900\r\nEXT:\r\nCACHE-CONTROL: max-age=100\r\n"
           "LOCATION: " + location(id) + "\r\n"
           "SERVER: Linux/3.14.0 UPnP/1.0 IpBridge/1.20.0\r\n"
           "hue-bridgeid: " + id.bridgeid + "\r\n"
           "ST: " + v.first + "\r\nUSN: " + v.second + "\r\n\r\n";
}

inline std::string notify(const bridge_identity::Identity& id, int variant) {
    const auto v = variants(id)[static_cast<size_t>(variant)];
    return "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=100\r\n"
           "LOCATION: " + location(id) + "\r\n"
           "SERVER: Linux/3.14.0 UPnP/1.0 IpBridge/1.20.0\r\nNTS: ssdp:alive\r\n"
           "hue-bridgeid: " + id.bridgeid + "\r\nNT: " + v.first + "\r\nUSN: " + v.second + "\r\n\r\n";
}

}  // namespace ssdp_text
