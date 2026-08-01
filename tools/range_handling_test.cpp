// How the downloader behaves against servers with different Range manners.
//
// Splitting a file across connections is only safe when the server honors
// Range. One that ignores it answers every worker with 200 and the whole body,
// and each worker writes that body at its own offset — a file nearly twice the
// right size, reported as finished. These cases pin down all three servers we
// can meet: honest, Range-ignoring, and one that claims Range on the probe and
// then ignores it.
//
// Build: enable ODM_BUILD_TESTS in CMake, then `ctest -C Release`.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "Downloader.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

int g_failures = 0;

void Check(const std::string& what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

enum class Manners {
    Honor,       // proper 206 + Content-Range
    Ignore,      // always 200 + the whole body, Range never honored
    LieOnProbe   // 206 for the probe's "0-0", then 200 + whole body
};

class Server {
public:
    explicit Server(Manners m, size_t size) : manners_(m), body_(size, '\0') {
        for (size_t i = 0; i < size; ++i) body_[i] = static_cast<char>(i % 251);
    }

    bool Start() {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(sock_, (sockaddr*)&a, sizeof(a)) || listen(sock_, 32)) return false;
        int len = sizeof(a);
        getsockname(sock_, (sockaddr*)&a, &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { Loop(); });
        return true;
    }

    void Stop() {
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        if (thread_.joinable()) thread_.join();
    }

    std::string Url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/file.bin";
    }
    const std::string& Body() const { return body_; }
    int Served() const { return served_.load(); }
    int Ranged() const { return ranged_.load(); }

private:
    void Loop() {
        for (;;) {
            SOCKET c = accept(sock_, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            std::thread([this, c] { Handle(c); }).detach();
        }
    }

    // "bytes=100-199" -> {100, 199}; end == -1 for an open range.
    static bool ParseRange(const std::string& v, long long* from, long long* to) {
        const size_t eq = v.find('=');
        if (eq == std::string::npos) return false;
        const size_t dash = v.find('-', eq);
        if (dash == std::string::npos) return false;
        *from = atoll(v.substr(eq + 1, dash - eq - 1).c_str());
        const std::string tail = v.substr(dash + 1);
        *to = tail.empty() ? -1 : atoll(tail.c_str());
        return true;
    }

    void Handle(SOCKET c) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) { closesocket(c); return; }
            req.append(buf, n);
        }
        ++served_;
        const bool head = req.rfind("HEAD ", 0) == 0;

        std::string low = req;
        for (char& ch : low) ch = (char)tolower((unsigned char)ch);
        long long from = 0, to = -1;
        bool ranged = false;
        const size_t rp = low.find("\r\nrange:");
        if (rp != std::string::npos) {
            const size_t vs = req.find(':', rp + 2) + 1;
            const size_t ve = req.find("\r\n", vs);
            std::string v = req.substr(vs, ve - vs);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            ranged = ParseRange(v, &from, &to);
            if (ranged) ++ranged_;
        }

        const long long total = (long long)body_.size();
        const bool probe = ranged && from == 0 && to == 0;
        bool honor = ranged &&
                     (manners_ == Manners::Honor ||
                      (manners_ == Manners::LieOnProbe && probe));

        std::string hdr, payload;
        if (honor) {
            const long long last = (to < 0 || to >= total) ? total - 1 : to;
            const long long len = last - from + 1;
            payload = body_.substr((size_t)from, (size_t)len);
            hdr = "HTTP/1.1 206 Partial Content\r\n"
                  "Content-Range: bytes " + std::to_string(from) + "-" +
                  std::to_string(last) + "/" + std::to_string(total) + "\r\n"
                  "Accept-Ranges: bytes\r\n";
        } else {
            payload = body_;
            hdr = "HTTP/1.1 200 OK\r\n";
        }
        hdr += "Content-Type: application/octet-stream\r\n"
               "Content-Length: " + std::to_string(payload.size()) + "\r\n"
               "Connection: close\r\n\r\n";

        send(c, hdr.data(), (int)hdr.size(), 0);
        if (!head) {
            size_t sent = 0;
            while (sent < payload.size()) {
                int n = send(c, payload.data() + sent,
                             (int)std::min<size_t>(65536, payload.size() - sent), 0);
                if (n <= 0) break;
                sent += (size_t)n;
            }
        }
        shutdown(c, SD_SEND);
        closesocket(c);
    }

    Manners     manners_;
    std::string body_;
    SOCKET      sock_ = INVALID_SOCKET;
    uint16_t    port_ = 0;
    std::thread thread_;
    std::atomic<int> served_{0};
    std::atomic<int> ranged_{0};
};

bool FileMatches(const std::string& path, const std::string& want) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    if ((size_t)f.tellg() != want.size()) return false;
    f.seekg(0);
    std::vector<char> got(want.size());
    f.read(got.data(), (std::streamsize)want.size());
    return std::equal(got.begin(), got.end(), want.begin());
}

long long FileSize(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    return f ? (long long)f.tellg() : -1;
}

void Cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".odmprog").c_str());
}

}  // namespace

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    constexpr size_t kBig   = 2 * 1024 * 1024;   // over the 1 MB split threshold
    constexpr size_t kSmall = 64 * 1024;         // under it

    // 1. The bug: a big file from a server that ignores Range.
    {
        std::printf("server ignores Range, file over the split threshold\n");
        Server srv(Manners::Ignore, kBig);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "rh_ignore.bin";
        Cleanup(out);

        odm::Downloader dl;
        dl.Start(srv.Url(), out, "t1");
        const odm::DownloadResult r = dl.WaitForCompletion();

        Check("download reports Completed",
              r.status == odm::DownloadStatus::Completed);
        Check("file is exactly the right size",
              FileSize(out) == (long long)kBig);
        Check("file is byte-for-byte correct", FileMatches(out, srv.Body()));
        // The probe is one request; the transfer must not fan out on top of it.
        Check("only one connection was used for the body",
              srv.Served() <= 3);
        std::printf("       size=%lld want=%zu  requests=%d\n",
                    FileSize(out), kBig, srv.Served());
        Cleanup(out);
        srv.Stop();
    }

    // 2. Must not over-correct: an honest server still gets many connections.
    {
        std::printf("server honors Range (multi-connection must still work)\n");
        Server srv(Manners::Honor, kBig);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "rh_honor.bin";
        Cleanup(out);

        odm::Downloader dl;
        dl.Start(srv.Url(), out, "t2");
        const odm::DownloadResult r = dl.WaitForCompletion();

        Check("download reports Completed",
              r.status == odm::DownloadStatus::Completed);
        Check("file is byte-for-byte correct", FileMatches(out, srv.Body()));
        Check("the file really was split across connections",
              srv.Served() > 3);
        std::printf("       requests=%d (ranged=%d)\n",
                    srv.Served(), srv.Ranged());
        Cleanup(out);
        srv.Stop();
    }

    // 3. A server that claims Range on the probe and then ignores it. We
    //    cannot get a correct file out of that, so the one thing that must
    //    never happen is calling it a success.
    {
        std::printf("server claims Range on the probe, then ignores it\n");
        Server srv(Manners::LieOnProbe, kBig);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "rh_liar.bin";
        Cleanup(out);

        odm::Downloader dl;
        dl.Start(srv.Url(), out, "t3");
        const odm::DownloadResult r = dl.WaitForCompletion();

        const bool completed = (r.status == odm::DownloadStatus::Completed);
        Check("a wrong file is never reported as Completed",
              !completed || FileMatches(out, srv.Body()));
        if (!completed)
            std::printf("       failed as expected: %s\n",
                        r.error_message.c_str());
        Cleanup(out);
        srv.Stop();
    }

    // 4. Small files take the single-connection path either way.
    {
        std::printf("small file, server ignores Range\n");
        Server srv(Manners::Ignore, kSmall);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "rh_small.bin";
        Cleanup(out);

        odm::Downloader dl;
        dl.Start(srv.Url(), out, "t4");
        const odm::DownloadResult r = dl.WaitForCompletion();

        Check("download reports Completed",
              r.status == odm::DownloadStatus::Completed);
        Check("file is byte-for-byte correct", FileMatches(out, srv.Body()));
        Cleanup(out);
        srv.Stop();
    }

    curl_global_cleanup();
    WSACleanup();
    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
