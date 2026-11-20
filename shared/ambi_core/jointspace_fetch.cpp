// v0.37: moved here from src/jointspace_client.cpp, unchanged apart from the
// Zone type (jointspace::Zone, which monitor_state::Zone now names) and a
// smaller body cap on the ESP32, so the Linux bridge and the AmbiBottom usermod
// run the same JointSpace client. Polling is read-only: one GET of /6/ambilight/processed
// on the TV's port 1926 (HTTPS, Digest auth). v0.45: the pairing (pair_request / pair_grant,
// at the bottom) is the only thing that sends anything to the TV, and only when the user
// starts it from the page.
#include "jointspace_fetch.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#if __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#include <cJSON.h>  // ESP-IDF "json" component
#endif

namespace jointspace {


namespace {

#ifdef ESP_PLATFORM
constexpr size_t kMaxBody = 48 * 1024;    // v0.37 ESP32: the real reply is a few KB; no 1 MB on a 320 KB heap
#else
constexpr size_t kMaxBody = 1024 * 1024;  // same cap as the Python original
#endif
constexpr const char* kPath = "/6/ambilight/processed";

std::string md5_hex(const std::string& in) {
    unsigned char digest[16];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    mbedtls_md(info, reinterpret_cast<const unsigned char*>(in.data()), in.size(), digest);
    char buf[33];
    for (int i = 0; i < 16; ++i) std::snprintf(buf + i * 2, 3, "%02x", digest[i]);
    return std::string(buf, 32);
}

std::string hex_random(mbedtls_ctr_drbg_context* drbg, int bytes) {
    std::vector<unsigned char> raw(bytes);
    mbedtls_ctr_drbg_random(drbg, raw.data(), raw.size());
    // 2 hex digits + NUL. The previous size here was `sizeof(buf) + 1` with
    // buf[2] - i.e. telling snprintf the buffer was 3 bytes when it was
    // only 2, so every single call wrote one byte past the end of `buf` on
    // the stack, onto whatever the compiler placed next (here, `out`'s own
    // fields). That's a deterministic one-byte stack overflow on every
    // byte of every nonce this function generates, not a rare race - a far
    // more likely source of "free(): invalid pointer" crashes than the
    // mDNS fork-hazard fix from the previous version, since it only
    // required js_thread (JointSpace/TV polling) to be active, which this
    // bridge's real-hardware runs do and the earlier mDNS-only stress test
    // in a sandbox never exercised.
    char buf[3];
    std::string out;
    for (auto b : raw) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

// Pulls one quoted-or-bare value for `key` out of a WWW-Authenticate /
// Authorization-style header value, e.g. key="value" or key=value.
bool extract_param(const std::string& header, const std::string& key, std::string& value) {
    size_t pos = header.find(key + "=");
    if (pos == std::string::npos) return false;
    pos += key.size() + 1;
    if (pos >= header.size()) return false;
    if (header[pos] == '"') {
        size_t end = header.find('"', pos + 1);
        if (end == std::string::npos) return false;
        value = header.substr(pos + 1, end - pos - 1);
    } else {
        size_t end = header.find_first_of(", \t\r\n", pos);
        value = header.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    }
    return true;
}

struct DigestChallenge {
    std::string realm, nonce, opaque;
    bool has_opaque = false;
    bool qop_auth = false;  // supports qop="auth" (vs. legacy no-qop digest)
};

bool parse_www_authenticate(const std::string& header, DigestChallenge& out) {
    // header looks like: Digest realm="...", nonce="...", qop="auth", opaque="..."
    if (header.compare(0, 7, "Digest ") != 0) return false;
    if (!extract_param(header, "realm", out.realm)) return false;
    if (!extract_param(header, "nonce", out.nonce)) return false;
    std::string qop;
    if (extract_param(header, "qop", qop) && qop.find("auth") != std::string::npos) out.qop_auth = true;
    out.has_opaque = extract_param(header, "opaque", out.opaque);
    return true;
}

// ---- minimal blocking TLS client (mbedTLS), one request/response per
// connection - see jointspace_client.hpp for why that's an intentional
// simplification versus the old bridge's persistent session. ------------
struct TlsConn {
    mbedtls_net_context fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool ok = false;

    TlsConn() {
        mbedtls_net_init(&fd);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~TlsConn() {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_net_free(&fd);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

// Connects, completes a TLS handshake with certificate verification
// disabled - same trust model as the Python original's `verify=False`:
// these are self-signed TV certificates on the local LAN, not something
// a public CA would ever sign, and this client never leaves the LAN.
bool tls_connect(TlsConn& c, const std::string& ip, int timeout_ms, std::string& error_code) {
    const char* pers = "jointspace_client";
    if (mbedtls_ctr_drbg_seed(&c.drbg, mbedtls_entropy_func, &c.entropy,
                               reinterpret_cast<const unsigned char*>(pers), std::strlen(pers)) != 0) {
        error_code = "request_failed";
        return false;
    }
    if (mbedtls_net_connect(&c.fd, ip.c_str(), "1926", MBEDTLS_NET_PROTO_TCP) != 0) {
        error_code = "network_unavailable";
        return false;
    }
    mbedtls_net_set_block(&c.fd);
    mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.drbg);
    if (mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        error_code = "request_failed";
        return false;
    }
    mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_read_timeout(&c.conf, static_cast<uint32_t>(timeout_ms));
    if (mbedtls_ssl_setup(&c.ssl, &c.conf) != 0) {
        error_code = "request_failed";
        return false;
    }
    mbedtls_ssl_set_bio(&c.ssl, &c.fd, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
    int ret;
    while ((ret = mbedtls_ssl_handshake(&c.ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            error_code = (ret == MBEDTLS_ERR_SSL_TIMEOUT) ? "network_timeout" : "network_unavailable";
            return false;
        }
    }
    c.ok = true;
    return true;
}

bool send_all(mbedtls_ssl_context* ssl, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = mbedtls_ssl_write(ssl, reinterpret_cast<const unsigned char*>(data.data() + sent), data.size() - sent);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

struct HttpResponse {
    int status = 0;
    std::string www_authenticate;
    bool server_closes = false;  // "Connection: close" - don't reuse this connection
    std::string body;
};

// Reads exactly one HTTP/1.1 response off an already-connected TLS socket:
// status line, headers, then a body framed by Content-Length, by chunked
// transfer-encoding, or (only if neither is present) by the server closing
// the connection. Reading exactly one framed response - and never past it -
// is what makes it safe to send the next request on the same connection
// (keep-alive, see Client below). Returns false on a transport-level
// failure/timeout or a malformed response.
bool read_http_response(mbedtls_ssl_context* ssl, HttpResponse& out, std::string& error_code) {
    std::string buf;
    unsigned char chunk[4096];
    // Pulls more bytes into `buf`. Returns 1 on data, 0 on clean EOF,
    // -1 on error (error_code set).
    auto fill = [&]() -> int {
        for (;;) {
            int n = mbedtls_ssl_read(ssl, chunk, sizeof(chunk));
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n == MBEDTLS_ERR_SSL_TIMEOUT) {
                error_code = "network_timeout";
                return -1;
            }
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            if (n < 0) {
                error_code = "network_unavailable";
                return -1;
            }
            buf.append(reinterpret_cast<char*>(chunk), static_cast<size_t>(n));
            if (buf.size() > kMaxBody + 65536) {
                error_code = "invalid_response";
                return -1;
            }
            return 1;
        }
    };

    size_t header_end;
    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos) {
        int r = fill();
        if (r <= 0) {
            if (r == 0) error_code = "network_unavailable";  // closed before a full header
            return false;
        }
    }
    std::string head = buf.substr(0, header_end);
    buf.erase(0, header_end + 4);

    size_t line_end = head.find("\r\n");
    std::string status_line = head.substr(0, line_end);
    size_t sp1 = status_line.find(' ');
    if (sp1 == std::string::npos) {
        error_code = "invalid_response";
        return false;
    }
    out.status = std::atoi(status_line.c_str() + sp1 + 1);

    long content_length = -1;
    bool chunked = false;
    size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
        size_t next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        std::string line = head.substr(pos, next - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return std::tolower(ch); });
            std::string lval = val;
            std::transform(lval.begin(), lval.end(), lval.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (key == "www-authenticate") out.www_authenticate = val;
            else if (key == "content-length") content_length = std::atol(val.c_str());
            else if (key == "transfer-encoding" && lval.find("chunked") != std::string::npos) chunked = true;
            else if (key == "connection" && lval.find("close") != std::string::npos) out.server_closes = true;
        }
        pos = next + 2;
    }

    if (chunked) {
        // <hex size>[;ext]\r\n<data>\r\n ... 0\r\n[trailers]\r\n
        for (;;) {
            size_t le;
            while ((le = buf.find("\r\n")) == std::string::npos) {
                if (fill() <= 0) {
                    if (error_code.empty()) error_code = "network_unavailable";
                    return false;
                }
            }
            long size = std::strtol(buf.substr(0, le).c_str(), nullptr, 16);
            buf.erase(0, le + 2);
            if (size < 0 || out.body.size() + static_cast<size_t>(size) > kMaxBody) {
                error_code = "invalid_response";
                return false;
            }
            if (size == 0) {
                // Trailers (normally none), terminated by an empty line.
                size_t te;
                while ((te = buf.find("\r\n")) == std::string::npos) {
                    if (fill() <= 0) {
                        if (error_code.empty()) error_code = "network_unavailable";
                        return false;
                    }
                }
                if (te == 0) break;
                buf.erase(0, te + 2);
                continue;
            }
            while (buf.size() < static_cast<size_t>(size) + 2) {
                if (fill() <= 0) {
                    if (error_code.empty()) error_code = "network_unavailable";
                    return false;
                }
            }
            out.body.append(buf, 0, static_cast<size_t>(size));
            buf.erase(0, static_cast<size_t>(size) + 2);
        }
    } else if (content_length >= 0) {
        if (static_cast<size_t>(content_length) > kMaxBody) {
            error_code = "invalid_response";
            return false;
        }
        while (buf.size() < static_cast<size_t>(content_length)) {
            int r = fill();
            if (r <= 0) {
                if (r == 0) error_code = "network_unavailable";  // closed mid-body
                return false;
            }
        }
        out.body = buf.substr(0, static_cast<size_t>(content_length));
    } else {
        // No framing at all: the body runs until the server closes.
        int r;
        while ((r = fill()) > 0) {
        }
        if (r < 0) return false;
        out.body = buf;
        out.server_closes = true;
    }
    return true;
}

// Validates and reshapes the TV's processed-ambilight JSON exactly like
// the Python original's processed_sides(): layer1.{left,top,right}, each
// an object of 1..512 pixels keyed by numeric index, each pixel {r,g,b}
// as an int 0..255. Anything else is rejected rather than trusted.
bool processed_sides_from_json(const std::string& body, Sides& out) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return false;
    bool ok = true;
    cJSON* layer1 = cJSON_GetObjectItemCaseSensitive(root, "layer1");
    if (!cJSON_IsObject(layer1)) ok = false;

    auto parse_side = [&](const char* name, std::vector<Zone>& zones) {
        if (!ok) return;
        cJSON* side = cJSON_GetObjectItemCaseSensitive(layer1, name);
        if (!cJSON_IsObject(side)) {
            ok = false;
            return;
        }
        // Collect numeric keys and sort ascending, same as the Python
        // `sorted(side, key=int)` - JSON object field order isn't
        // guaranteed to already be numeric order.
        std::vector<std::pair<int, cJSON*>> indexed;
        cJSON* pixel = nullptr;
        cJSON_ArrayForEach(pixel, side) {
            if (!pixel->string) {
                ok = false;
                return;
            }
            char* endp = nullptr;
            long idx = std::strtol(pixel->string, &endp, 10);
            if (endp == pixel->string || *endp != '\0') {
                ok = false;
                return;
            }
            indexed.emplace_back(static_cast<int>(idx), pixel);
        }
        if (indexed.empty() || indexed.size() > 512) {
            ok = false;
            return;
        }
        std::sort(indexed.begin(), indexed.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [idx, p] : indexed) {
            (void)idx;
            cJSON* r = cJSON_GetObjectItemCaseSensitive(p, "r");
            cJSON* g = cJSON_GetObjectItemCaseSensitive(p, "g");
            cJSON* b = cJSON_GetObjectItemCaseSensitive(p, "b");
            if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b)) {
                ok = false;
                return;
            }
            auto in_range = [](double v) { return v == static_cast<int>(v) && v >= 0 && v <= 255; };
            if (!in_range(r->valuedouble) || !in_range(g->valuedouble) || !in_range(b->valuedouble)) {
                ok = false;
                return;
            }
            Zone z;
            z.r = static_cast<uint8_t>(r->valuedouble);
            z.g = static_cast<uint8_t>(g->valuedouble);
            z.b = static_cast<uint8_t>(b->valuedouble);
            zones.push_back(z);
        }
    };
    parse_side("left", out.left);
    parse_side("top", out.top);
    parse_side("right", out.right);
    cJSON_Delete(root);
    return ok;
}

bool looks_like_ip(const std::string& s) {
    // Just enough validation to reject garbage before opening a socket -
    // mbedtls_net_connect would also fail on these, but with a less
    // specific error; a real syntax check isn't worth a dependency here.
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)) && c != '.' && c != ':') return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------
// Persistent client (v7). Until v6, every poll opened a brand-new TCP+TLS
// connection, sent an unauthenticated request just to collect a fresh
// Digest challenge (401), then sent the real request and closed - i.e. a
// full TLS handshake plus two requests per poll, 5 polls a second at the
// default interval. Measured against a mock TV: 30 TLS handshakes and 60
// requests in 10 s. The TV's small JointSpace HTTPS server stops accepting
// connections on port 1926 after a while of that, which is exactly the
// "the TV needs a restart to recover, the old bridge never needed that"
// behaviour seen on the real 65OLED707. The old Python bridge used a
// requests.Session: one kept-alive connection, and the Digest nonce reused
// across requests with an incrementing nonce count - after the first poll,
// one request per poll and no handshakes at all. This does the same.
// ---------------------------------------------------------------------

struct Client::Impl {
    std::unique_ptr<TlsConn> conn;  // heap-allocated: mbedTLS keeps pointers into it (bio)
    std::string ip, user, pass;
    DigestChallenge challenge;
    bool have_challenge = false;
    uint32_t nc = 0;

    void drop_connection() { conn.reset(); }
    void reset_all() {
        conn.reset();
        have_challenge = false;
        nc = 0;
    }

    std::string auth_header() {
        std::string ha1 = md5_hex(user + ":" + challenge.realm + ":" + pass);
        std::string ha2 = md5_hex(std::string("GET:") + kPath);
        std::string h;
        if (challenge.qop_auth) {
            ++nc;
            char ncbuf[9];
            std::snprintf(ncbuf, sizeof(ncbuf), "%08x", nc);
            std::string cnonce = hex_random(&conn->drbg, 8);
            std::string response = md5_hex(ha1 + ":" + challenge.nonce + ":" + ncbuf + ":" + cnonce + ":auth:" + ha2);
            h = "Digest username=\"" + user + "\", realm=\"" + challenge.realm + "\", nonce=\"" + challenge.nonce +
                "\", uri=\"" + kPath + "\", qop=auth, nc=" + ncbuf + ", cnonce=\"" + cnonce + "\", response=\"" +
                response + "\"";
        } else {
            std::string response = md5_hex(ha1 + ":" + challenge.nonce + ":" + ha2);
            h = "Digest username=\"" + user + "\", realm=\"" + challenge.realm + "\", nonce=\"" + challenge.nonce +
                "\", uri=\"" + kPath + "\", response=\"" + response + "\"";
        }
        if (challenge.has_opaque) h += ", opaque=\"" + challenge.opaque + "\"";
        return h;
    }
};

Client::Client() : impl_(new Impl) {}
Client::~Client() = default;
void Client::reset() { impl_->reset_all(); }

bool Client::fetch(const std::string& tv_ip, const std::string& username, const std::string& password, Sides& out,
                   std::string& error_code, int timeout_ms) {
    Impl& m = *impl_;
    if (!looks_like_ip(tv_ip)) {
        error_code = "invalid_address";
        return false;
    }
    if (tv_ip != m.ip || username != m.user || password != m.pass) {
        m.reset_all();
        m.ip = tv_ip;
        m.user = username;
        m.pass = password;
    }

    // At most: one retry on a fresh connection if a reused one turns out to
    // have been closed by the TV meanwhile, plus one re-challenge if the
    // TV rotated its nonce. Never an unbounded loop.
    bool sent_auth_with_fresh_challenge = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        bool reused = static_cast<bool>(m.conn);
        if (!m.conn) {
            m.conn.reset(new TlsConn);
            if (!tls_connect(*m.conn, tv_ip, timeout_ms, error_code)) {
                m.drop_connection();
                return false;
            }
        }
        std::string req = "GET " + std::string(kPath) + " HTTP/1.1\r\nHost: " + tv_ip +
                          ":1926\r\nAccept: application/json\r\n";
        if (m.have_challenge) req += "Authorization: " + m.auth_header() + "\r\n";
        req += "\r\n";

        HttpResponse resp;
        std::string err;
        if (!send_all(&m.conn->ssl, req) || !read_http_response(&m.conn->ssl, resp, err)) {
            m.drop_connection();
            if (reused) continue;  // idle keep-alive connection the TV had closed: one retry, fresh
            error_code = err.empty() ? "network_unavailable" : err;
            return false;
        }
        if (resp.server_closes) m.drop_connection();

        if (resp.status == 401) {
            DigestChallenge fresh;
            if (!parse_www_authenticate(resp.www_authenticate, fresh)) {
                error_code = "authentication_failed";
                return false;
            }
            bool stale = resp.www_authenticate.find("stale=true") != std::string::npos ||
                         resp.www_authenticate.find("stale=TRUE") != std::string::npos;
            // Credentials were just tried against a challenge we'd only
            // now received, and the TV still says no (and not because the
            // nonce expired): genuinely wrong username/password.
            if (sent_auth_with_fresh_challenge && !stale) {
                m.have_challenge = false;
                error_code = "authentication_failed";
                return false;
            }
            m.challenge = fresh;
            m.have_challenge = true;
            m.nc = 0;
            sent_auth_with_fresh_challenge = true;
            continue;
        }
        if (resp.status == 403) {
            error_code = "forbidden";
            return false;
        }
        if (resp.status != 200) {
            error_code = "request_failed";
            return false;
        }
        if (!processed_sides_from_json(resp.body, out)) {
            error_code = "invalid_response";
            return false;
        }
        return true;
    }
    error_code = "request_failed";
    return false;
}

bool fetch_processed_sides(const std::string& tv_ip, const std::string& username, const std::string& password,
                            Sides& out, std::string& error_code, int timeout_ms) {
    Client c;
    return c.fetch(tv_ip, username, password, out, error_code, timeout_ms);
}

// ---- v0.45: pairing --------------------------------------------------------------------------
namespace {

// The shared key every JointSpace client uses for the pairing signature (public; from
// ha-philipsjs, MIT: AUTH_SHARED_KEY). Base64 as published there.
constexpr const char* kPairKeyB64 = "ZmVay1EQVFOaZhwQ4Kv81ypLAZNczV9sG4KkseXWn1NEk6cXmPKO/MCa9sryslvLCFMnNe4Z4CPXzToowvhHvA==";

std::string b64(const unsigned char* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::string out(olen, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out.size(), &olen, data, len) != 0) return "";
    out.resize(olen);
    return out;
}

std::string device_json(const PairState& st) {
    cJSON* d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "device_name", st.device_name.c_str());
    cJSON_AddStringToObject(d, "device_os", "Android");
    cJSON_AddStringToObject(d, "type", "native");
    cJSON_AddStringToObject(d, "id", st.device_id.c_str());
    cJSON_AddStringToObject(d, "app_id", "app.ambibottom");
    cJSON_AddStringToObject(d, "app_name", "AmbiBottom");
    char* t = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    std::string out = t ? t : "{}";
    if (t) cJSON_free(t);
    return out;
}

// One request on a fresh connection; with credentials: Digest after the TV's 401.
// v0.46: any method (GET without a body)
bool request_json(const std::string& tv_ip, const std::string& method, const std::string& path, const std::string& body,
                  const std::string& user, const std::string& pass, int& status, std::string& reply, std::string& error_code,
                  int timeout_ms) {
    if (!looks_like_ip(tv_ip)) {
        error_code = "invalid_address";
        return false;
    }
    DigestChallenge ch;
    bool have = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::unique_ptr<TlsConn> c(new TlsConn);
        if (!tls_connect(*c, tv_ip, timeout_ms, error_code)) return false;
        std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + tv_ip + ":1926\r\nAccept: application/json\r\n";
        if (method != "GET") req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        if (have) {
            const std::string ha1 = md5_hex(user + ":" + ch.realm + ":" + pass);
            const std::string ha2 = md5_hex(method + ":" + path);
            std::string h = "Digest username=\"" + user + "\", realm=\"" + ch.realm + "\", nonce=\"" + ch.nonce + "\", uri=\"" + path + "\"";
            if (ch.qop_auth) {
                const std::string cnonce = hex_random(&c->drbg, 8);
                h += ", qop=auth, nc=00000001, cnonce=\"" + cnonce + "\", response=\"" +
                     md5_hex(ha1 + ":" + ch.nonce + ":00000001:" + cnonce + ":auth:" + ha2) + "\"";
            } else {
                h += ", response=\"" + md5_hex(ha1 + ":" + ch.nonce + ":" + ha2) + "\"";
            }
            if (ch.has_opaque) h += ", opaque=\"" + ch.opaque + "\"";
            req += "Authorization: " + h + "\r\n";
        }
        req += "Connection: close\r\n\r\n";
        if (method != "GET") req += body;
        HttpResponse resp;
        std::string err;
        if (!send_all(&c->ssl, req) || !read_http_response(&c->ssl, resp, err)) {
            error_code = err.empty() ? "network_unavailable" : err;
            return false;
        }
        status = resp.status;
        if (status == 401 && !user.empty() && !have) {
            if (!parse_www_authenticate(resp.www_authenticate, ch)) {
                error_code = "authentication_failed";
                return false;
            }
            have = true;
            continue;
        }
        reply = resp.body;
        return true;
    }
    error_code = "authentication_failed";
    return false;
}

// "SUCCESS" -> true; else error_code = the TV's error_id in lower case
bool tv_success(const std::string& reply, cJSON** out, std::string& error_code) {
    cJSON* j = cJSON_Parse(reply.c_str());
    if (!cJSON_IsObject(j)) {
        if (j) cJSON_Delete(j);
        error_code = "invalid_response";
        return false;
    }
    const cJSON* e = cJSON_GetObjectItemCaseSensitive(j, "error_id");
    if (!cJSON_IsString(e) || std::string(e->valuestring) != "SUCCESS") {
        std::string id = cJSON_IsString(e) ? e->valuestring : "request_failed";
        for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        error_code = id;
        cJSON_Delete(j);
        return false;
    }
    *out = j;
    return true;
}

}  // namespace

std::string pair_signature(const std::string& timestamp, const std::string& pin) {
    unsigned char key[96];
    size_t klen = 0;
    if (mbedtls_base64_decode(key, sizeof(key), &klen, reinterpret_cast<const unsigned char*>(kPairKeyB64),
                              std::strlen(kPairKeyB64)) != 0)
        return "";
    const std::string msg = timestamp + pin;
    unsigned char mac[20];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (mbedtls_md_hmac(info, key, klen, reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), mac) != 0) return "";
    return b64(mac, sizeof(mac));
}

bool pair_request(const std::string& tv_ip, const std::string& device_name, PairState& st, std::string& error_code,
                  int timeout_ms) {
    st = PairState();
    st.device_name = device_name.empty() ? "AmbiBottom" : device_name;
    {
        mbedtls_entropy_context ent;
        mbedtls_ctr_drbg_context drbg;
        mbedtls_entropy_init(&ent);
        mbedtls_ctr_drbg_init(&drbg);
        const char* pers = "jointspace_pair";
        const bool ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, reinterpret_cast<const unsigned char*>(pers),
                                              std::strlen(pers)) == 0;
        if (ok) st.device_id = hex_random(&drbg, 16);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&ent);
        if (!ok) {
            error_code = "request_failed";
            return false;
        }
    }
    const std::string body = "{\"access\":{\"scope\":[\"read\",\"write\",\"control\"]},\"device\":" + device_json(st) + "}";
    int status = 0;
    std::string reply;
    if (!request_json(tv_ip, "POST", "/6/pair/request", body, "", "", status, reply, error_code, timeout_ms)) return false;
    cJSON* j = nullptr;
    if (!tv_success(reply, &j, error_code)) return false;
    const cJSON* ts = cJSON_GetObjectItemCaseSensitive(j, "timestamp");
    const cJSON* key = cJSON_GetObjectItemCaseSensitive(j, "auth_key");
    const cJSON* to = cJSON_GetObjectItemCaseSensitive(j, "timeout");
    bool ok = cJSON_IsString(key) && (cJSON_IsNumber(ts) || cJSON_IsString(ts));
    if (ok) {
        st.auth_key = key->valuestring;
        if (cJSON_IsNumber(ts)) {
            char b[32];
            std::snprintf(b, sizeof(b), "%.0f", ts->valuedouble);
            st.timestamp = b;
        } else {
            st.timestamp = ts->valuestring;
        }
        st.timeout_s = cJSON_IsNumber(to) ? static_cast<int>(to->valuedouble) : 60;
        // it goes into the grant's JSON as a bare number: digits only
        ok = !st.timestamp.empty() && st.timestamp.size() < 24 &&
             std::all_of(st.timestamp.begin(), st.timestamp.end(), [](char c) { return c >= '0' && c <= '9'; });
    }
    cJSON_Delete(j);
    if (!ok) error_code = "invalid_response";
    return ok;
}

bool pair_grant(const std::string& tv_ip, const PairState& st, const std::string& pin, std::string& error_code,
                int timeout_ms) {
    const std::string sig = pair_signature(st.timestamp, pin);
    if (sig.empty()) {
        error_code = "request_failed";
        return false;
    }
    cJSON* pin_s = cJSON_CreateString(pin.c_str());  // escaped properly whatever was typed
    char* pin_j = cJSON_PrintUnformatted(pin_s);
    cJSON_Delete(pin_s);
    const std::string body = "{\"auth\":{\"auth_appId\":\"1\",\"auth_timestamp\":" + st.timestamp + ",\"auth_signature\":\"" + sig +
                             "\",\"pin\":" + std::string(pin_j ? pin_j : "\"\"") + "},\"device\":" + device_json(st) + "}";
    if (pin_j) cJSON_free(pin_j);
    int status = 0;
    std::string reply;
    if (!request_json(tv_ip, "POST", "/6/pair/grant", body, st.device_id, st.auth_key, status, reply, error_code, timeout_ms))
        return false;
    if (status == 401) {
        error_code = "authentication_failed";
        return false;
    }
    cJSON* j = nullptr;
    if (!tv_success(reply, &j, error_code)) return false;
    cJSON_Delete(j);
    return true;
}


}  // namespace jointspace
