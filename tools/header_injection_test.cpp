// Extra request headers must not be able to end their own header line.
//
// A hand-off may carry extra headers, and they arrive as JSON — whose string
// escapes can spell out a carriage return. libcurl does not police the header
// lines it is handed, so "a\r\nX-Injected: 1" does not stay one value: the
// tail becomes another header, and a blank line followed by a body becomes a
// whole second request, sent to the same host with the same cookies attached.
//
// Checked here at both ends: HeaderPairSafe as a unit, and the real bridge
// over a socket, so a payload carrying a split attempt is observed as the app
// would actually receive it.
//
// Build: enable ODM_BUILD_TESTS in CMake, then `ctest -C Release`.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "BridgeServer.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr uint16_t kPort = 47932;   // not 47923, and not bridge_host's 47931

int g_failures = 0;

void Check(const std::string& what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("  (" + detail + ")").c_str());
    if (!ok) ++g_failures;
}

std::string Send(const std::string& request) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof(a))) { closesocket(s); return {}; }
    send(s, request.data(), (int)request.size(), 0);
    shutdown(s, SD_SEND);
    std::string resp;
    char buf[4096];
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, n);
    }
    closesocket(s);
    return resp;
}

int StatusOf(const std::string& resp) {
    if (resp.size() < 12 || resp.rfind("HTTP/1.1 ", 0) != 0) return 0;
    return std::atoi(resp.c_str() + 9);
}

std::string PostAdd(const std::string& token, const std::string& body) {
    std::string r = "POST /add HTTP/1.1\r\n";
    r += "Host: 127.0.0.1:" + std::to_string(kPort) + "\r\n";
    r += "X-ODM-Token: " + token + "\r\n";
    r += "Content-Type: application/json\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Connection: close\r\n\r\n" + body;
    return Send(r);
}

// The payloads the bridge handed to the app, in order.
std::mutex g_mtx;
std::vector<odm::BridgePayload> g_payloads;

odm::BridgePayload LastPayload() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_payloads.empty() ? odm::BridgePayload{} : g_payloads.back();
}

bool HasHeaderNamed(const odm::BridgePayload& p, const std::string& name) {
    for (const auto& kv : p.headers) if (kv.first == name) return true;
    return false;
}

std::string HeaderValue(const odm::BridgePayload& p, const std::string& name) {
    for (const auto& kv : p.headers) if (kv.first == name) return kv.second;
    return {};
}

// Would this pair, rendered the way ApplyRequestContext renders it, introduce
// a second header line? This is the property that actually matters.
bool RendersToOneLine(const std::string& name, const std::string& value) {
    const std::string line = name + ": " + value;
    return line.find('\r') == std::string::npos &&
           line.find('\n') == std::string::npos;
}

}  // namespace

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::printf("the rule, on its own\n");
    Check("an ordinary header is allowed",
          odm::HeaderPairSafe("X-Requested-With", "XMLHttpRequest"));
    Check("so is one with punctuation in the value",
          odm::HeaderPairSafe("Authorization", "Bearer abc.def-ghi_jkl/mno=="));
    Check("and a token name using the odd characters RFC 7230 permits",
          odm::HeaderPairSafe("X-Weird!#$%&'*+-.^_`|~1", "fine"));
    Check("a tab inside a value is still a value",
          odm::HeaderPairSafe("X-Tabbed", "a\tb"));

    Check("a CR in the value is refused",
          !odm::HeaderPairSafe("X-Evil", "a\rX-Injected: 1"));
    Check("an LF in the value is refused",
          !odm::HeaderPairSafe("X-Evil", "a\nX-Injected: 1"));
    Check("a full CRLF split is refused",
          !odm::HeaderPairSafe("X-Evil", "a\r\nX-Injected: 1"));
    Check("a smuggled second request is refused",
          !odm::HeaderPairSafe(
              "X-Evil", "a\r\n\r\nGET /admin HTTP/1.1\r\nHost: victim\r\n\r\n"));
    Check("a NUL in the value is refused",
          !odm::HeaderPairSafe("X-Evil", std::string("a\0b", 3)));
    Check("a bare control character is refused",
          !odm::HeaderPairSafe("X-Evil", "a\x01b"));

    Check("a newline in the NAME is refused",
          !odm::HeaderPairSafe("X-Evil\r\nX-Injected", "1"));
    Check("a colon in the name is refused",
          !odm::HeaderPairSafe("X-Evil: injected\r\nX-Other", "1"));
    Check("a space in the name is refused",
          !odm::HeaderPairSafe("X Evil", "1"));
    Check("an empty name is refused", !odm::HeaderPairSafe("", "1"));
    Check("a leading space in the value is refused (obs-fold)",
          !odm::HeaderPairSafe("X-Fold", " continued"));
    Check("an absurdly long value is refused",
          !odm::HeaderPairSafe("X-Big", std::string(64 * 1024, 'a')));
    Check("an absurdly long name is refused",
          !odm::HeaderPairSafe(std::string(500, 'a'), "1"));
    Check("an empty value is fine", odm::HeaderPairSafe("X-Empty", ""));

    std::printf("the same rule, through the real bridge\n");
    odm::BridgeServer server;
    if (!server.Start(kPort, [](const odm::BridgePayload& p) {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_payloads.push_back(p);
        })) {
        std::printf("could not bind 127.0.0.1:%u\n", kPort);
        return 1;
    }
    const std::string token = server.Token();

    {
        // \r\n written as JSON escapes, which is how it would really arrive.
        const std::string body =
            "{\"url\":\"https://example.test/f.bin\","
            "\"headers\":{"
            "\"X-Ok\":\"kept\","
            "\"X-Evil\":\"a\\r\\nX-Injected: 1\","
            "\"X-Evil2\":\"b\\nX-Injected2: 2\""
            "}}";
        Check("the hand-off is accepted", StatusOf(PostAdd(token, body)) == 200);
        const odm::BridgePayload p = LastPayload();

        Check("the harmless header survives", HasHeaderNamed(p, "X-Ok"));
        Check("  with its value intact", HeaderValue(p, "X-Ok") == "kept");
        Check("the CRLF header is dropped", !HasHeaderNamed(p, "X-Evil"));
        Check("the LF header is dropped", !HasHeaderNamed(p, "X-Evil2"));
        Check("nothing was silently rewritten into a new header",
              !HasHeaderNamed(p, "X-Injected") &&
              !HasHeaderNamed(p, "X-Injected2"));
        std::printf("       kept %zu of 3 headers\n", p.headers.size());
    }
    {
        // A name that ends its own line is the other half of the same trick.
        const std::string body =
            "{\"url\":\"https://example.test/f.bin\","
            "\"headers\":{\"X-Name\\r\\nX-Injected\":\"1\"}}";
        Check("a split attempt in the NAME is accepted but dropped",
              StatusOf(PostAdd(token, body)) == 200);
        const odm::BridgePayload p = LastPayload();
        Check("  no header came through at all", p.headers.empty(),
              std::to_string(p.headers.size()) + " headers");
    }
    {
        // The invariant, stated as the thing that actually matters: whatever
        // survives must render as exactly one header line.
        const std::string body =
            "{\"url\":\"https://example.test/f.bin\","
            "\"headers\":{"
            "\"Referer\":\"https://example.test/page\","
            "\"X-Bad\":\"v\\r\\nHost: evil.example\","
            "\"Accept\":\"*/*\""
            "}}";
        Check("a mixed batch is accepted", StatusOf(PostAdd(token, body)) == 200);
        const odm::BridgePayload p = LastPayload();
        bool all_single_line = true;
        for (const auto& kv : p.headers)
            if (!RendersToOneLine(kv.first, kv.second)) all_single_line = false;
        Check("every surviving header renders as one line", all_single_line);
        Check("  and the good ones are still there",
              HasHeaderNamed(p, "Referer") && HasHeaderNamed(p, "Accept"));
        Check("  while the bad one is not", !HasHeaderNamed(p, "X-Bad"));
    }

    server.Stop();
    WSACleanup();
    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
