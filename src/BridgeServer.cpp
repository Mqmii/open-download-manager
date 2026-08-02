// _CRT_RAND_S must be defined before the FIRST inclusion of <stdlib.h>
// (BridgeServer.h's standard includes may already pull it in).
#define _CRT_RAND_S
#include <stdlib.h>

#include "BridgeServer.h"

// The quality menu is answered straight from here: ListHeights() is a pure
// query with no app state behind it, so routing it through ODMApp would only
// add a hop.
#include "YtDlp.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

// Defined by CMake from project(VERSION). The fallback only matters to a
// build that compiles this file outside the project's own target.
#ifndef ODM_VERSION
  #define ODM_VERSION "0.0.0"
#endif

namespace odm {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Minimal flat-JSON parser
// ---------------------------------------------------------------------------

namespace {

struct JsonCursor {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }
    bool peek(char c) { skip_ws(); return p < end && *p == c; }
    bool eat(char c) {
        skip_ws();
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }
};

// Append a Unicode code point as UTF-8.
static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static bool ParseHex4(JsonCursor& c, uint32_t& out) {
    if (c.end - c.p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        char ch = *c.p++;
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= static_cast<uint32_t>(ch - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

// Parse a JSON string (cursor must be on the opening quote) and unescape it.
static bool ParseJsonString(JsonCursor& c, std::string& out) {
    if (!c.eat('"')) return false;
    out.clear();
    while (c.p < c.end) {
        char ch = *c.p++;
        if (ch == '"') return true;
        if (ch == '\\') {
            if (c.p >= c.end) return false;
            char esc = *c.p++;
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(c, cp)) return false;
                    // Combine UTF-16 surrogate pairs.
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        c.end - c.p >= 6 && c.p[0] == '\\' && c.p[1] == 'u') {
                        uint32_t lo = 0;
                        JsonCursor save = c;
                        c.p += 2;
                        if (ParseHex4(c, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            c = save;   // not a pair: emit the lone surrogate
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        } else {
            out += ch;   // raw byte (JSON is UTF-8, pass through)
        }
    }
    return false;   // unterminated string
}

// Skip a non-string JSON value (number, true, false, null, or a whole
// array/object we don't care about). Returns false on malformed input.
static bool SkipJsonValue(JsonCursor& c, int depth = 0) {
    if (depth > 16) return false;
    c.skip_ws();
    if (c.p >= c.end) return false;
    if (c.eat('{')) {
        c.skip_ws();
        if (c.eat('}')) return true;
        while (true) {
            std::string key;
            if (!ParseJsonString(c, key)) return false;
            if (!c.eat(':')) return false;
            if (c.peek('"')) {
                std::string v;
                if (!ParseJsonString(c, v)) return false;
            } else if (!SkipJsonValue(c, depth + 1)) {
                return false;
            }
            if (c.eat(',')) continue;
            return c.eat('}');
        }
    }
    if (c.eat('[')) {
        c.skip_ws();
        if (c.eat(']')) return true;
        while (true) {
            if (c.peek('"')) {
                std::string v;
                if (!ParseJsonString(c, v)) return false;
            } else if (!SkipJsonValue(c, depth + 1)) {
                return false;
            }
            if (c.eat(',')) continue;
            return c.eat(']');
        }
    }
    // Literal: consume until a delimiter.
    const char* start = c.p;
    while (c.p < c.end && *c.p != ',' && *c.p != '}' && *c.p != ']' &&
           *c.p != ' ' && *c.p != '\t' && *c.p != '\r' && *c.p != '\n')
        ++c.p;
    return c.p > start;
}

// Parse one object level; nested objects recurse with a "parent." prefix.
static bool ParseObjectLevel(JsonCursor& c,
                             std::map<std::string, std::string>& out,
                             const std::string& prefix,
                             int depth = 0) {
    if (depth > 4 || !c.eat('{')) return false;
    c.skip_ws();
    if (c.eat('}')) return true;
    while (true) {
        std::string key;
        if (!ParseJsonString(c, key)) return false;
        if (!c.eat(':')) return false;
        if (c.peek('"')) {
            std::string v;
            if (!ParseJsonString(c, v)) return false;
            out[prefix + key] = v;
        } else if (c.peek('{')) {
            if (!ParseObjectLevel(c, out, prefix + key + ".", depth + 1))
                return false;
        } else if (!SkipJsonValue(c)) {
            return false;
        }
        if (c.eat(',')) continue;
        return c.eat('}');
    }
}

} // namespace

bool OriginIsOurExtension(const std::string& origin) {
    // Exact match only. A prefix test would let
    // "chrome-extension://pmhd...aeh.evil" through, and an id is fixed-length
    // anyway, so there is nothing to be lenient about.
    static const std::string ours =
        std::string("chrome-extension://") + kExtensionId;
    return origin == ours;
}

bool HeaderPairSafe(const std::string& name, const std::string& value) {
    // Generous, but not unbounded: real headers are short, and the only
    // reason to send a huge one is to make somebody else's parser unhappy.
    constexpr size_t kMaxName  = 128;
    constexpr size_t kMaxValue = 4 * 1024;
    if (name.empty() || name.size() > kMaxName) return false;
    if (value.size() > kMaxValue) return false;

    // RFC 7230 token: no separators, no spaces, no controls. This rejects the
    // colon too, so a name cannot smuggle a second field in by itself.
    for (const unsigned char c : name) {
        const bool token =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
            c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
            c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
        if (!token) return false;
    }

    // Values may hold anything printable (and a tab), but no line structure:
    // CR or LF ends the field, and a NUL truncates it for whoever reads it as
    // a C string on the way out.
    for (const unsigned char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
        if (c < 0x20 && c != '\t') return false;
    }

    // A value that begins with whitespace is an obs-fold continuation of the
    // header before it rather than a value of its own; curl would send it,
    // and what the far end makes of it is anyone's guess.
    if (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        return false;

    return true;
}

bool ParseSimpleJson(const std::string& json,
                     std::map<std::string, std::string>& out) {
    JsonCursor c{json.data(), json.data() + json.size()};
    if (!ParseObjectLevel(c, out, "")) return false;
    c.skip_ws();
    return c.p == c.end;   // trailing garbage is an error
}

// ---------------------------------------------------------------------------
// Token: persisted next to the executable so restarts don't break the
// extension's stored token (it re-fetches via /ping anyway).
// ---------------------------------------------------------------------------

namespace {

std::string GenerateToken() {
    unsigned int vals[8] = {0};
    for (auto& v : vals) rand_s(&v);
    static const char* hex = "0123456789abcdef";
    std::string t;
    t.reserve(64);
    for (unsigned int v : vals)
        for (int i = 7; i >= 0; --i)
            t += hex[(v >> (i * 4)) & 0xF];
    return t;
}

std::string LoadOrCreateToken() {
#if defined(_WIN32)
    wchar_t exe[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        fs::path tok_path = fs::path(exe).parent_path() / "bridge.token";
        // Reuse an existing valid token (64 lowercase hex chars).
        std::error_code ec;
        auto sz = fs::file_size(tok_path, ec);
        if (!ec && sz >= 64) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, tok_path.c_str(), L"rb") == 0 && f) {
                std::string t(64, '\0');
                size_t n = fread(t.data(), 1, 64, f);
                fclose(f);
                bool ok = (n == 64) &&
                    std::all_of(t.begin(), t.end(), [](char c) {
                        return std::isxdigit(static_cast<unsigned char>(c));
                    });
                if (ok) return t;
            }
        }
        // First run: generate and persist.
        std::string t = GenerateToken();
        FILE* f = nullptr;
        if (_wfopen_s(&f, tok_path.c_str(), L"wb") == 0 && f) {
            fwrite(t.data(), 1, t.size(), f);
            fclose(f);
        }
        return t;
    }
#endif
    return GenerateToken();   // fallback: per-run token
}

} // namespace

// ---------------------------------------------------------------------------
// BridgeServer
// ---------------------------------------------------------------------------

BridgeServer::~BridgeServer() {
    Stop();
}

bool BridgeServer::Start(uint16_t port, AddCallback cb) {
#if !defined(_WIN32)
    (void)port; (void)cb;
    return false;   // POSIX implementation not provided yet.
#else
    if (running_.load()) return true;
    cb_ = std::move(cb);
    token_ = LoadOrCreateToken();

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    wsa_started_ = true;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { WSACleanup(); wsa_started_ = false; return false; }

    // Prevent another process from hijacking the port later.
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&excl), sizeof(excl));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only
    addr.sin_port = htons(port);

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(s, 8) == SOCKET_ERROR) {
        closesocket(s);
        WSACleanup();
        wsa_started_ = false;
        return false;
    }

    listen_sock_ = static_cast<uintptr_t>(s);
    port_ = port;
    running_.store(true);
    thread_ = std::thread(&BridgeServer::ListenLoop, this);
    return true;
#endif
}

void BridgeServer::Stop() {
#if defined(_WIN32)
    running_.store(false);
    if (listen_sock_ != static_cast<uintptr_t>(~0ull)) {
        // Closing the socket unblocks accept() in the listener thread.
        closesocket(static_cast<SOCKET>(listen_sock_));
        listen_sock_ = static_cast<uintptr_t>(~0ull);
    }
    if (thread_.joinable()) thread_.join();
    if (wsa_started_) { WSACleanup(); wsa_started_ = false; }
#endif
}

void BridgeServer::ListenLoop() {
#if defined(_WIN32)
    SOCKET ls = static_cast<SOCKET>(listen_sock_);
    while (running_.load()) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (!running_.load()) break;      // socket closed by Stop()
            continue;                          // transient error, keep listening
        }
        // Never let a stalled client jam the listener.
        DWORD tv = 5000;   // 5s recv/send timeout
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv));
        HandleClient(static_cast<uintptr_t>(c));
        closesocket(c);
    }
#endif
}

// ---------------------------------------------------------------------------
// HTTP handling
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxBodyBytes   = 64 * 1024;

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers; // lower-cased keys
    std::string body;
};

static bool SendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent,
                     static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// Read one request (headers + optional Content-Length body). Returns false
// on any protocol error / timeout / oversize.
static bool ReadRequest(SOCKET s, HttpRequest& req) {
    std::string raw;
    raw.reserve(4096);
    char buf[4096];

    // 1) Read until end of headers.
    while (raw.find("\r\n\r\n") == std::string::npos) {
        if (raw.size() > kMaxHeaderBytes) return false;
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        raw.append(buf, static_cast<size_t>(n));
    }
    size_t hdr_end = raw.find("\r\n\r\n");
    std::string head = raw.substr(0, hdr_end);
    std::string body = raw.substr(hdr_end + 4);

    // 2) Request line: METHOD SP PATH SP VERSION
    size_t sp1 = head.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = head.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    req.method = head.substr(0, sp1);
    req.path   = head.substr(sp1 + 1, sp2 - sp1 - 1);

    // 3) Header lines.
    size_t pos = head.find("\r\n");
    if (pos == std::string::npos) return false;
    pos += 2;
    while (pos < head.size()) {
        size_t eol = head.find("\r\n", pos);
        if (eol == std::string::npos) eol = head.size();
        std::string line = head.substr(pos, eol - pos);
        pos = eol + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        req.headers[ToLower(line.substr(0, colon))] = Trim(line.substr(colon + 1));
    }

    // 4) Body per Content-Length (fetch always sends one for POST JSON).
    size_t want = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        try { want = static_cast<size_t>(std::stoull(it->second)); }
        catch (...) { return false; }
        if (want > kMaxBodyBytes) return false;
    }
    while (body.size() < want) {
        int n = recv(s, buf, static_cast<int>(
            std::min(sizeof(buf), want - body.size())), 0);
        if (n <= 0) return false;
        body.append(buf, static_cast<size_t>(n));
    }
    req.body = body.substr(0, want);
    return true;
}

// Is this request addressed to us by our own name, or did it arrive under
// somebody else's?
//
// Binding to 127.0.0.1 keeps remote machines out, but it does not keep a web
// page out: an attacker can point their own domain at 127.0.0.1 (DNS
// rebinding), at which point the browser treats this server as same-origin
// with their page and hands them every response — CORS never even applies,
// because from the browser's side nothing is cross-origin. What still gives
// the trick away is the Host header, which carries the name the client typed
// (`evil.example:47923`) rather than the address it resolved to. Anything but
// our own loopback name is refused.
static bool HostIsOurs(const std::string& host, uint16_t port) {
    if (host.empty()) return false;   // HTTP/1.1 requires it

    std::string name = host;
    std::string port_part;
    if (!name.empty() && name[0] == '[') {          // [::1]:47923
        const size_t close = name.find(']');
        if (close == std::string::npos) return false;
        if (close + 1 < name.size()) {
            if (name[close + 1] != ':') return false;
            port_part = name.substr(close + 2);
        }
        name = name.substr(1, close - 1);
    } else {
        const size_t colon = name.rfind(':');
        if (colon != std::string::npos) {
            port_part = name.substr(colon + 1);
            name = name.substr(0, colon);
        }
    }

    // A port, when spelled out, has to be the one we are listening on: a page
    // reaching us on another port is not talking to this server.
    if (!port_part.empty()) {
        if (port_part.find_first_not_of("0123456789") != std::string::npos)
            return false;
        if (std::atoi(port_part.c_str()) != static_cast<int>(port)) return false;
    }

    name = ToLower(name);
    return name == "127.0.0.1" || name == "localhost" || name == "::1";
}

static void Respond(SOCKET s, int code, const std::string& reason,
                    const std::string& body,
                    const std::string& allow_origin = std::string(),
                    const std::string& extra_headers = std::string()) {
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    r += "Content-Type: application/json\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!allow_origin.empty())
        r += "Access-Control-Allow-Origin: " + allow_origin + "\r\n";
    r += "Cache-Control: no-store\r\n";
    if (!extra_headers.empty()) r += extra_headers;
    r += "Connection: close\r\n\r\n";
    r += body;
    SendAll(s, r);
}

} // namespace

void BridgeServer::HandleClient(uintptr_t client) {
#if defined(_WIN32)
    SOCKET s = static_cast<SOCKET>(client);
    HttpRequest req;
    if (!ReadRequest(s, req)) return;   // garbage/timeout: drop silently

    // Before anything else, and for every endpoint: a request that reached us
    // under a foreign name is a rebinding attempt, not a client of ours.
    {
        auto hit = req.headers.find("host");
        const std::string host = hit == req.headers.end() ? std::string()
                                                          : hit->second;
        if (!HostIsOurs(host, port_)) {
            Respond(s, 403, "Forbidden", "{\"ok\":false,\"error\":\"bad host\"}");
            return;
        }
    }

    // CORS: only OUR extension's origin gets echoed. Web pages get nothing,
    // which makes the browser block them from reading our responses — and so
    // does any other extension, which used to be waved through because every
    // `chrome-extension://` origin looked alike before the id was pinned.
    std::string origin;
    auto oit = req.headers.find("origin");
    if (oit != req.headers.end()) origin = oit->second;
    const bool ext_origin = OriginIsOurExtension(origin);
    const std::string allow_origin = ext_origin ? origin : std::string();

    // --- CORS preflight ----------------------------------------------------
    if (req.method == "OPTIONS") {
        std::string extra;
        if (ext_origin) {
            extra += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
            extra += "Access-Control-Allow-Headers: Content-Type, X-ODM-Token\r\n";
            extra += "Access-Control-Max-Age: 86400\r\n";
        }
        Respond(s, 204, "No Content", "", allow_origin, extra);
        return;
    }

    // --- GET /ping ---------------------------------------------------------
    // Doubles as the handshake and as "is ODM running?". That ODM is running
    // is not a secret; the token is, so it only rides along for a caller that
    // is either our own extension or presents no Origin at all (a local
    // process, which can read bridge.token off the disk regardless). A
    // DIFFERENT extension gets the version and nothing else.
    if (req.method == "GET" && (req.path == "/ping" || req.path == "/")) {
        std::string body = "{\"app\":\"odm\",\"version\":\"" ODM_VERSION "\"";
        if (origin.empty() || ext_origin)
            body += ",\"token\":\"" + token_ + "\"";
        body += "}";
        Respond(s, 200, "OK", body, allow_origin);
        return;
    }

    // --- POST /add ---------------------------------------------------------
    if (req.method == "POST" && req.path == "/add") {
        // Defense in depth: an Origin header that isn't ours means a web page,
        // a rebinding attack, or another extension — refuse outright. Absent
        // is still allowed: Chrome omits Origin for a caller that holds
        // loopback host permission, which our own extension does.
        if (!origin.empty() && !ext_origin) {
            Respond(s, 403, "Forbidden", "{\"ok\":false,\"error\":\"bad origin\"}");
            return;
        }
        auto tit = req.headers.find("x-odm-token");
        if (tit == req.headers.end() || tit->second != token_) {
            Respond(s, 401, "Unauthorized", "{\"ok\":false,\"error\":\"bad token\"}");
            return;
        }

        std::map<std::string, std::string> m;
        if (!ParseSimpleJson(req.body, m)) {
            Respond(s, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad json\"}");
            return;
        }
        BridgePayload p;
        p.url        = m["url"];
        p.filename   = m["filename"];
        p.referrer   = m["referrer"];
        p.cookies    = m["cookies"];
        p.cookie_jar = m["cookieJar"];
        p.user_agent = m["userAgent"];
        p.audio_url  = m["audioUrl"];
        p.type       = m["type"];
        p.height     = m["height"];
        // Dropped, not rejected: a caller sending one unusable header should
        // still get its download. What must not happen is the pair reaching
        // the wire, where a CR in the value would split the request.
        for (const auto& kv : m) {
            if (kv.first.rfind("headers.", 0) != 0) continue;
            const std::string hname = kv.first.substr(8);
            if (HeaderPairSafe(hname, kv.second))
                p.headers.emplace_back(hname, kv.second);
        }
        // Only real downloads: plain http(s) URLs.
        if (p.url.rfind("http://", 0) != 0 && p.url.rfind("https://", 0) != 0) {
            Respond(s, 400, "Bad Request",
                    "{\"ok\":false,\"error\":\"unsupported url\"}");
            return;
        }
        if (cb_) cb_(p);
        Respond(s, 200, "OK", "{\"ok\":true}", allow_origin);
        return;
    }

    // --- POST /ytformats ---------------------------------------------------
    // The quality menu the in-page panel shows for a YouTube video. Answering
    // means running yt-dlp, which takes a second or two, and this server
    // handles one connection at a time — so the extension caches the answer
    // per video and asks once, not on every hover.
    if (req.method == "POST" && req.path == "/ytformats") {
        // Same origin rule as /add.
        if (!origin.empty() && !ext_origin) {
            Respond(s, 403, "Forbidden", "{\"ok\":false,\"error\":\"bad origin\"}");
            return;
        }
        auto tit = req.headers.find("x-odm-token");
        if (tit == req.headers.end() || tit->second != token_) {
            Respond(s, 401, "Unauthorized", "{\"ok\":false,\"error\":\"bad token\"}");
            return;
        }
        std::map<std::string, std::string> m;
        if (!ParseSimpleJson(req.body, m)) {
            Respond(s, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad json\"}");
            return;
        }
        std::vector<int> heights;
        ytdlp::ListHeights(m["url"], &heights);
        std::string body = "{\"ok\":true,\"heights\":[";
        for (size_t i = 0; i < heights.size(); ++i) {
            if (i) body += ",";
            body += std::to_string(heights[i]);
        }
        body += "]}";
        Respond(s, 200, "OK", body, allow_origin);
        return;
    }

    Respond(s, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}");
#else
    (void)client;
#endif
}

} // namespace odm
