// Host-header check for the browser bridge.
//
// Runs the real BridgeServer on loopback and speaks raw HTTP to it, because
// the point of the check is a header no HTTP client will let you forge. A
// page that has pointed its own domain at 127.0.0.1 (DNS rebinding) reaches
// this server same-origin — CORS does not apply — and the Host header is what
// still tells the two apart.
//
// Build: enable ODM_BUILD_TESTS in CMake, then `ctest -C Release`.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "BridgeServer.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr uint16_t kPort = 47931;   // not 47923: a running ODM must not clash

int g_failures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Send one raw request, return the whole response ("" on connect failure).
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

// "HTTP/1.1 403 Forbidden\r\n..." -> 403
int StatusOf(const std::string& resp) {
    if (resp.size() < 12 || resp.rfind("HTTP/1.1 ", 0) != 0) return 0;
    return std::atoi(resp.c_str() + 9);
}

std::string Get(const std::string& path, const std::string& host_header) {
    std::string r = "GET " + path + " HTTP/1.1\r\n";
    if (!host_header.empty()) r += "Host: " + host_header + "\r\n";
    r += "Connection: close\r\n\r\n";
    return Send(r);
}

std::string Post(const std::string& path, const std::string& host_header,
                 const std::string& token, const std::string& body,
                 const std::string& origin = std::string()) {
    std::string r = "POST " + path + " HTTP/1.1\r\n";
    if (!host_header.empty()) r += "Host: " + host_header + "\r\n";
    if (!origin.empty()) r += "Origin: " + origin + "\r\n";
    if (!token.empty()) r += "X-ODM-Token: " + token + "\r\n";
    r += "Content-Type: application/json\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Connection: close\r\n\r\n" + body;
    return Send(r);
}

}  // namespace

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::atomic<int> adds{0};
    odm::BridgeServer server;
    if (!server.Start(kPort, [&adds](const odm::BridgePayload&) { ++adds; })) {
        std::printf("could not bind 127.0.0.1:%u\n", kPort);
        return 1;
    }
    const std::string token = server.Token();
    const std::string port  = std::to_string(kPort);

    std::printf("the names we answer to\n");
    {
        const std::string r = Get("/ping", "127.0.0.1:" + port);
        Check("GET /ping via 127.0.0.1:port -> 200", StatusOf(r) == 200);
        Check("  and it still carries the token",
              r.find(token) != std::string::npos);
    }
    Check("GET /ping via localhost:port -> 200",
          StatusOf(Get("/ping", "localhost:" + port)) == 200);
    Check("GET /ping via [::1]:port -> 200",
          StatusOf(Get("/ping", "[::1]:" + port)) == 200);
    Check("GET /ping via a bare 127.0.0.1 -> 200",
          StatusOf(Get("/ping", "127.0.0.1")) == 200);

    std::printf("names that are not ours (rebinding)\n");
    for (const char* host : {"evil.example", "attacker.test",
                             "odm.localhost", "127.0.0.1.evil.example",
                             "localhost.evil.example"}) {
        const std::string r = Get("/ping", std::string(host) + ":" + port);
        char label[160];
        std::snprintf(label, sizeof(label),
                      "GET /ping via %s -> 403 and no token", host);
        Check(label, StatusOf(r) == 403 && r.find(token) == std::string::npos);
    }
    Check("GET / (the alias) via evil.example -> 403",
          StatusOf(Get("/", "evil.example:" + port)) == 403);
    Check("GET /ping with no Host at all -> 403",
          StatusOf(Get("/ping", "")) == 403);
    Check("GET /ping on somebody else's port -> 403",
          StatusOf(Get("/ping", "127.0.0.1:1234")) == 403);
    Check("GET /ping with a junk port -> 403",
          StatusOf(Get("/ping", "127.0.0.1:notaport")) == 403);

    std::printf("the check does not break the hand-off\n");
    {
        const std::string body = "{\"url\":\"https://example.test/f.bin\"}";
        const std::string r = Post("/add", "127.0.0.1:" + port, token, body,
                                   "chrome-extension://abcdefghijklmnop");
        Check("POST /add from the extension -> 200", StatusOf(r) == 200);
        Check("  and the payload reached the app", adds.load() == 1);
    }
    {
        const std::string body = "{\"url\":\"https://example.test/evil.bin\"}";
        const std::string r = Post("/add", "evil.example:" + port, token, body);
        Check("POST /add under a foreign Host -> 403", StatusOf(r) == 403);
        Check("  and nothing was queued", adds.load() == 1);
    }
    Check("OPTIONS preflight via 127.0.0.1:port -> 204",
          StatusOf(Send("OPTIONS /add HTTP/1.1\r\nHost: 127.0.0.1:" + port +
                        "\r\nOrigin: chrome-extension://abcdefghijklmnop\r\n"
                        "Connection: close\r\n\r\n")) == 204);
    Check("OPTIONS preflight under a foreign Host -> 403",
          StatusOf(Send("OPTIONS /add HTTP/1.1\r\nHost: evil.example:" + port +
                        "\r\nOrigin: chrome-extension://abcdefghijklmnop\r\n"
                        "Connection: close\r\n\r\n")) == 403);

    std::printf("the pre-existing gates still hold\n");
    Check("POST /add with a wrong token -> 401",
          StatusOf(Post("/add", "127.0.0.1:" + port, "deadbeef",
                        "{\"url\":\"https://example.test/f.bin\"}")) == 401);
    Check("POST /add from a web page origin -> 403",
          StatusOf(Post("/add", "127.0.0.1:" + port, token,
                        "{\"url\":\"https://example.test/f.bin\"}",
                        "https://evil.example")) == 403);
    Check("nothing extra was queued", adds.load() == 1);

    server.Stop();
    WSACleanup();
    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
