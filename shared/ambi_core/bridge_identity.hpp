// Non-secret bridge identity: the MAC, derived bridgeid/UDN, and the
// description.xml / short_config fields the TV expects. None of this is a
// pairing secret - it's the same kind of information printed on the
// bottom of a real Hue bridge. Ported from fakebridge.py's Ident class,
// keeping the exact same derivation so the TV (which keys on MAC/bridgeid,
// not on IP) keeps recognising this bridge across runs.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace bridge_identity {

// The user's own already-working fakebridge.py default - the TV only
// reconnects to a bridge whose MAC equals the one it stored at pairing, so
// this stays fixed rather than being randomly generated per run.
inline const std::string kDefaultMac = "00:17:88:a4:5e:01";
inline const std::string kDefaultName = "FakeBridge-A45E01";

struct Identity {
    std::string ip;       // this host's LAN IP, filled in at startup
    uint16_t port = 80;
    std::string mac;       // lowercase, colon-separated, e.g. "00:17:88:a4:5e:01"
    std::string name;
    std::string bridgeid;  // 16 hex chars, uppercase: mac[0:6] + "FFFE" + mac[6:12]
    std::string serial;    // mac with no separators, lowercase
    std::string udn;        // "2f402f80-da50-11e1-9b23-" + serial
    std::string apiversion = "1.16.0";
    std::string swversion = "01039019";
};

inline std::string mac_to_hex(const std::string& mac) {
    std::string hex;
    for (char c : mac) {
        if (c != ':') hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return hex;
}

inline Identity make_identity(const std::string& ip, uint16_t port,
                               const std::string& mac = kDefaultMac,
                               const std::string& name = kDefaultName) {
    Identity id;
    id.ip = ip;
    id.port = port;
    id.mac = mac;
    id.name = name;
    id.serial = mac_to_hex(mac);
    std::string bridgeid = id.serial.substr(0, 6) + "fffe" + id.serial.substr(6, 6);
    std::transform(bridgeid.begin(), bridgeid.end(), bridgeid.begin(),
                    [](unsigned char c) { return std::toupper(c); });
    id.bridgeid = bridgeid;
    id.udn = "2f402f80-da50-11e1-9b23-" + id.serial;
    return id;
}

inline std::string description_xml(const Identity& id) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
           "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
           "<specVersion>\n<major>1</major>\n<minor>0</minor>\n</specVersion>\n"
           "<URLBase>http://" + id.ip + ":" + std::to_string(id.port) + "/</URLBase>\n"
           "<device>\n"
           "<deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>\n"
           "<friendlyName>" + id.name + "</friendlyName>\n"
           "<manufacturer>Signify</manufacturer>\n"
           "<manufacturerURL>http://www.meethue.com</manufacturerURL>\n"
           "<modelDescription>Philips hue Personal Wireless Lighting</modelDescription>\n"
           "<modelName>Philips hue bridge 2015</modelName>\n"
           "<modelNumber>BSB002</modelNumber>\n"
           "<modelURL>http://www.meethue.com</modelURL>\n"
           "<serialNumber>" + id.serial + "</serialNumber>\n"
           "<UDN>uuid:" + id.udn + "</UDN>\n"
           "<presentationURL>index.html</presentationURL>\n"
           "</device>\n</root>\n";
}

}  // namespace bridge_identity
