// Cookie scoping check for the browser hand-off.
//
// Builds the real odm::ApplyRequestContext (Downloader.cpp) against two
// throwaway HTTP servers on loopback, given distinct hostnames via
// CURLOPT_RESOLVE so curl's cookie matching sees a genuine host boundary.
// Each case redirects from the first server to the second and asserts which
// of them was shown the Cookie header.
//
// Build (from the repo root, Developer Prompt or via cmake --build):
//   cl /std:c++17 /EHsc /I src /I C:/vcpkg/installed/x64-windows/include \
//      tools/cookie_scope_test.cpp src/Downloader.cpp \
//      C:/vcpkg/installed/x64-windows/lib/libcurl.lib ws2_32.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "Downloader.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

// A single-shot HTTP server: serves `reply` once, records the request it saw.
struct MiniServer {
    SOCKET      listener = INVALID_SOCKET;
    uint16_t    port     = 0;
    std::string reply;
    std::string seen_cookie;   // the Cookie header value, "" when absent
    std::atomic<bool> hit{false};
    std::thread thread;

    bool Listen() {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;                       // let the OS pick
        if (bind(listener, (sockaddr*)&a, sizeof(a)) || listen(listener, 4))
            return false;
        int len = sizeof(a);
        if (getsockname(listener, (sockaddr*)&a, &len)) return false;
        port = ntohs(a.sin_port);
        return true;
    }

    // Serves until Stop(); a redirect target may be asked more than once.
    void Serve() {
        thread = std::thread([this] {
            for (;;) {
                SOCKET c = accept(listener, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                std::string req;
                char buf[2048];
                for (;;) {
                    int n = recv(c, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    req.append(buf, n);
                    if (req.find("\r\n\r\n") != std::string::npos) break;
                }
                // Case-insensitive scan for the Cookie header.
                std::string low = req;
                for (char& ch : low) ch = (char)tolower((unsigned char)ch);
                size_t p = low.find("\r\ncookie:");
                if (p != std::string::npos) {
                    size_t vs = req.find(':', p + 2) + 1;
                    size_t ve = req.find("\r\n", vs);
                    seen_cookie = req.substr(vs, ve - vs);
                    while (!seen_cookie.empty() && seen_cookie[0] == ' ')
                        seen_cookie.erase(0, 1);
                }
                hit.store(true);
                send(c, reply.data(), (int)reply.size(), 0);
                shutdown(c, SD_SEND);
                closesocket(c);
            }
        });
    }

    void Stop() {
        if (listener != INVALID_SOCKET) closesocket(listener);
        listener = INVALID_SOCKET;
        if (thread.joinable()) thread.join();
    }

    void Reset() { seen_cookie.clear(); hit.store(false); }
};

int g_failures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// One run: origin server 302s to `target_host`, both sides record what they got.
void RunCase(const char* name, const odm::RequestContext& ctx,
             const char* origin_host, const char* target_host,
             bool expect_origin_cookie, bool expect_target_cookie,
             bool legacy_static_header = false) {
    MiniServer origin, target;
    if (!origin.Listen() || !target.Listen()) {
        std::printf("  [FAIL] %s: could not open loopback sockets\n", name);
        ++g_failures;
        return;
    }
    const std::string target_url = "http://" + std::string(target_host) + ":" +
                                   std::to_string(target.port) + "/next";
    origin.reply = "HTTP/1.1 302 Found\r\nLocation: " + target_url +
                   "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    target.reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                   "Connection: close\r\n\r\nok";
    origin.Serve();
    target.Serve();

    const std::string url = "http://" + std::string(origin_host) + ":" +
                            std::to_string(origin.port) + "/file";

    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     +[](char*, size_t s, size_t n, void*) { return s * n; });

    // Both fake hostnames resolve to loopback, so curl believes it is talking
    // to two different sites while everything stays in this process.
    curl_slist* resolve = nullptr;
    resolve = curl_slist_append(resolve,
        (std::string(origin_host) + ":" + std::to_string(origin.port) +
         ":127.0.0.1").c_str());
    resolve = curl_slist_append(resolve,
        (std::string(target_host) + ":" + std::to_string(target.port) +
         ":127.0.0.1").c_str());
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);

    if (legacy_static_header) {
        // The pre-fix behavior, kept as a control: proves the test would have
        // caught the leak.
        curl_easy_setopt(curl, CURLOPT_COOKIE, ctx.cookies.c_str());
    } else {
        odm::ApplyRequestContext(curl, ctx, url, nullptr);
    }

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(resolve);

    std::printf("%s\n", name);
    Check("transfer completed", rc == CURLE_OK);
    Check("redirect was followed", target.hit.load());
    if (expect_origin_cookie)
        Check("origin host received the cookie", !origin.seen_cookie.empty());
    else
        Check("origin host received NO cookie", origin.seen_cookie.empty());
    if (expect_target_cookie)
        Check("redirect target received the cookie", !target.seen_cookie.empty());
    else
        Check("redirect target received NO cookie", target.seen_cookie.empty());
    if (!target.seen_cookie.empty())
        std::printf("       target saw: Cookie: %s\n", target.seen_cookie.c_str());

    origin.Stop();
    target.Stop();
}

std::string JarLine(const char* domain, bool tail, const char* name,
                    const char* value) {
    return std::string(domain) + "\t" + (tail ? "TRUE" : "FALSE") + "\t/\t" +
           "FALSE\t0\t" + name + "\t" + value;
}

}  // namespace

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // 1. The vulnerability: a session cookie for site.test must not follow a
    //    redirect to evil.test.
    {
        odm::RequestContext ctx;
        ctx.cookies    = "session=secret";
        ctx.cookie_jar = JarLine(".site.test", true, "session", "secret");
        RunCase("cross-site redirect (site.test -> evil.test), scoped jar", ctx,
                "site.test", "evil.test",
                /*origin*/ true, /*target*/ false);
    }

    // 2. Control: the old static-header behavior, same setup. Expected to
    //    hand the cookie to evil.test — that is the bug being fixed.
    {
        odm::RequestContext ctx;
        ctx.cookies = "session=secret";
        RunCase("cross-site redirect, PRE-FIX CURLOPT_COOKIE (control)", ctx,
                "site.test", "evil.test",
                /*origin*/ true, /*target*/ true,
                /*legacy*/ true);
    }

    // 3. Must not over-block: a redirect to the site's own CDN subdomain is
    //    the normal case and has to keep working.
    {
        odm::RequestContext ctx;
        ctx.cookies    = "session=secret";
        ctx.cookie_jar = JarLine(".site.test", true, "session", "secret");
        RunCase("same-site redirect (site.test -> cdn.site.test), scoped jar",
                ctx, "site.test", "cdn.site.test",
                /*origin*/ true, /*target*/ true);
    }

    // 4. A host-only cookie stays on its exact host, as in the browser.
    {
        odm::RequestContext ctx;
        ctx.cookies    = "session=secret";
        ctx.cookie_jar = JarLine("www.site.test", false, "session", "secret");
        RunCase("host-only cookie, redirect to a sibling subdomain", ctx,
                "www.site.test", "cdn.site.test",
                /*origin*/ true, /*target*/ false);
    }

    // 5. Fallback path: only the flat header was supplied (old extension, or
    //    a context typed into the UI). Scoped to the captured host.
    {
        odm::RequestContext ctx;
        ctx.cookies = "session=secret; theme=dark";
        RunCase("header-only fallback, cross-site redirect", ctx,
                "site.test", "evil.test",
                /*origin*/ true, /*target*/ false);
    }
    {
        odm::RequestContext ctx;
        ctx.cookies = "session=secret; theme=dark";
        RunCase("header-only fallback, same-site redirect", ctx,
                "site.test", "cdn.site.test",
                /*origin*/ true, /*target*/ true);
    }

    // 6. A Secure cookie is withheld from a plaintext hop, as in the browser.
    //    These two lines are verbatim what the extension emits.
    {
        odm::RequestContext ctx;
        ctx.cookies    = "session=secret";
        ctx.cookie_jar = ".site.test\tTRUE\t/\tTRUE\t0\tsession\tsecret";
        RunCase("Secure cookie over http (same-site redirect)", ctx,
                "site.test", "cdn.site.test",
                /*origin*/ false, /*target*/ false);
    }
    {
        odm::RequestContext ctx;
        ctx.cookies    = "csrf=abc123";
        ctx.cookie_jar = ".site.test\tTRUE\t/\tFALSE\t1900000000\tcsrf\tabc123";
        RunCase("non-Secure cookie with a real expiry, same-site redirect", ctx,
                "site.test", "cdn.site.test",
                /*origin*/ true, /*target*/ true);
    }

    curl_global_cleanup();
    WSACleanup();
    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
