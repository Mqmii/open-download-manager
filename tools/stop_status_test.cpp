// Pressing Stop must report "stopped", never "failed".
//
// The UI treats the two completely differently: a failed row is auto-retried
// on a backoff, a stopped one is left alone. The DASH and yt-dlp engines both
// used to reach their exit path with the status still at its Failed default
// and only the message changed to "Download stopped." — so cancelling an
// Instagram or YouTube download made it start itself again seconds later.
//
// Covered here: the shared FinalizeCancelled rule as a unit, and the whole
// DashDownloader path against a loopback server that trickles bytes slowly
// enough for Stop() to land while the transfer is genuinely in flight.
//
// Build: enable ODM_BUILD_TESTS in CMake, then `ctest -C Release`.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "DashDownloader.h"
#include "Downloader.h"
#include "HlsDownloader.h"
#include <bcrypt.h>
#include <fstream>
#pragma comment(lib, "bcrypt.lib")

#pragma comment(lib, "ws2_32.lib")

namespace {

int g_failures = 0;

void Check(const std::string& what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

const char* StatusName(odm::DownloadStatus s) {
    switch (s) {
        case odm::DownloadStatus::Completed: return "Completed";
        case odm::DownloadStatus::Stopped:   return "Stopped";
        default:                             return "Failed";
    }
}

// A Range-honoring server that hands the body over in small slices with a
// pause between them, so a transfer stays in flight long enough to be
// cancelled on purpose rather than by luck. `missing` answers 404 instead,
// which is how the "a real failure is still a failure" case is driven.
class SlowServer {
public:
    SlowServer(size_t size, bool missing = false)
        : body_(size, '\0'), missing_(missing) {
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
        closing_.store(true);
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        if (thread_.joinable()) thread_.join();
        // Let the detached per-connection handlers notice and unwind.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    std::string Url(const char* name) const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/" + name;
    }
    // How many requests asked for a path containing `needle`.
    int HitsFor(const std::string& needle) const {
        std::lock_guard<std::mutex> lk(paths_mtx_);
        int n = 0;
        for (const auto& p : paths_)
            if (p.find(needle) != std::string::npos) ++n;
        return n;
    }

private:
    void Loop() {
        for (;;) {
            SOCKET c = accept(sock_, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            std::thread([this, c] { Handle(c); }).detach();
        }
    }

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
        const bool head = req.rfind("HEAD ", 0) == 0;
        {
            const size_t sp1 = req.find(' ');
            const size_t sp2 = req.find(' ', sp1 + 1);
            std::lock_guard<std::mutex> lk(paths_mtx_);
            paths_.push_back(req.substr(sp1 + 1, sp2 - sp1 - 1));
        }

        if (missing_) {
            const std::string r = "HTTP/1.1 404 Not Found\r\n"
                                  "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send(c, r.data(), (int)r.size(), 0);
            shutdown(c, SD_SEND);
            closesocket(c);
            return;
        }

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
        }

        const long long total = (long long)body_.size();
        std::string hdr, payload;
        if (ranged) {
            const long long last = (to < 0 || to >= total) ? total - 1 : to;
            payload = body_.substr((size_t)from, (size_t)(last - from + 1));
            hdr = "HTTP/1.1 206 Partial Content\r\n"
                  "Content-Range: bytes " + std::to_string(from) + "-" +
                  std::to_string(last) + "/" + std::to_string(total) + "\r\n"
                  "Accept-Ranges: bytes\r\n";
        } else {
            payload = body_;
            hdr = "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n";
        }
        hdr += "Content-Type: application/octet-stream\r\n"
               "Content-Length: " + std::to_string(payload.size()) + "\r\n"
               "Connection: close\r\n\r\n";
        send(c, hdr.data(), (int)hdr.size(), 0);

        // The probe's 1-byte range must come back at once or Start() would
        // spend the whole test window inside Probe() instead of downloading.
        if (!head && payload.size() > 2) {
            size_t sent = 0;
            while (sent < payload.size() && !closing_.load()) {
                const int want = (int)std::min<size_t>(4096, payload.size() - sent);
                const int n = send(c, payload.data() + sent, want, 0);
                if (n <= 0) break;
                sent += (size_t)n;
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
        } else if (!head) {
            send(c, payload.data(), (int)payload.size(), 0);
        }
        shutdown(c, SD_SEND);
        closesocket(c);
    }

    std::string body_;
    bool        missing_;
    SOCKET      sock_ = INVALID_SOCKET;
    uint16_t    port_ = 0;
    std::thread thread_;
    std::atomic<bool> closing_{false};
    mutable std::mutex       paths_mtx_;
    std::vector<std::string> paths_;
};

// Run one DashDownloader job and cancel it after `stop_after_ms`. Returns the
// result the engine reported to its completion callback.
odm::DownloadResult RunAndStop(const std::string& video_url,
                               const std::string& audio_url,
                               const std::string& out,
                               int stop_after_ms) {
    odm::DashDownloader dash;
    std::atomic<bool> done{false};
    odm::DownloadResult got;
    dash.SetCompletionCallback([&](const odm::DownloadResult& r) {
        got = r;
        done.store(true);
    });
    dash.Start(video_url, audio_url, out, "stop-test", odm::RequestContext{});
    std::this_thread::sleep_for(std::chrono::milliseconds(stop_after_ms));
    dash.Stop();
    dash.WaitForCompletion();
    // WaitForCompletion joins the worker, which fires the callback before it
    // returns; the flag is a guard against a silent engine, not a wait.
    Check("  the engine answered at all", done.load());
    return got;
}

void Cleanup(const std::string& path) {
    for (const char* suffix : {"", ".odmprog", ".vtrk", ".vtrk.odmprog",
                               ".atrk", ".atrk.odmprog"})
        std::remove((path + suffix).c_str());
}

}  // namespace

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // A body big enough that 4 KB slices at 40ms each cannot drain inside the
    // test's stop window, so the cancel always lands mid-transfer.
    constexpr size_t kBody = 4 * 1024 * 1024;

    std::printf("the rule itself\n");
    {
        odm::DownloadResult r;
        r.status = odm::DownloadStatus::Failed;
        r.error_message = "Video track download failed: 404";
        odm::FinalizeCancelled(r, true);
        Check("a cancelled job becomes Stopped",
              r.status == odm::DownloadStatus::Stopped);
        Check("  and says so", r.error_message == "Download stopped.");
    }
    {
        odm::DownloadResult r;
        r.status = odm::DownloadStatus::Failed;
        r.error_message = "connection reset";
        odm::FinalizeCancelled(r, false);
        Check("a genuine failure stays Failed",
              r.status == odm::DownloadStatus::Failed);
        Check("  and keeps its reason", r.error_message == "connection reset");
    }
    {
        // A Stop that arrives after the last byte landed did not undo it.
        odm::DownloadResult r;
        r.status = odm::DownloadStatus::Completed;
        odm::FinalizeCancelled(r, true);
        Check("a job that already finished stays Completed",
              r.status == odm::DownloadStatus::Completed);
    }

    std::printf("stopping a single-track DASH job\n");
    {
        SlowServer srv(kBody);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ss_single.mp4";
        Cleanup(out);

        const odm::DownloadResult r = RunAndStop(srv.Url("video.mp4"), "", out, 600);
        Check("Stop reports Stopped, not Failed",
              r.status == odm::DownloadStatus::Stopped);
        Check("  and the UI is told it was stopped",
              r.error_message == "Download stopped.");
        std::printf("       status=%s msg=\"%s\"\n",
                    StatusName(r.status), r.error_message.c_str());

        Cleanup(out);
        srv.Stop();
    }

    std::printf("stopping a paired (video+audio) DASH job\n");
    {
        SlowServer srv(kBody);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ss_paired.mp4";
        Cleanup(out);

        const odm::DownloadResult r =
            RunAndStop(srv.Url("video.mp4"), srv.Url("audio.mp4"), out, 600);
        Check("Stop during the video track reports Stopped",
              r.status == odm::DownloadStatus::Stopped);
        // The regression this pins: Downloader::Start clears its own stop
        // flag, so a cancel that landed between the two tracks used to be
        // forgotten and the audio track downloaded anyway.
        Check("  and the audio track was never requested",
              srv.HitsFor("audio.mp4") == 0);
        std::printf("       status=%s  video hits=%d audio hits=%d\n",
                    StatusName(r.status), srv.HitsFor("video.mp4"),
                    srv.HitsFor("audio.mp4"));

        Cleanup(out);
        srv.Stop();
    }

    std::printf("a job nobody stopped still fails\n");
    {
        SlowServer srv(kBody, /*missing=*/true);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ss_404.mp4";
        Cleanup(out);

        odm::DashDownloader dash;
        std::atomic<bool> done{false};
        odm::DownloadResult got;
        dash.SetCompletionCallback([&](const odm::DownloadResult& r) {
            got = r;
            done.store(true);
        });
        dash.Start(srv.Url("gone.mp4"), "", out, "fail-test",
                   odm::RequestContext{});
        dash.WaitForCompletion();

        Check("the engine answered at all", done.load());
        Check("a 404 is still reported as Failed",
              got.status == odm::DownloadStatus::Failed);
        Check("  so the UI may still auto-retry it",
              got.error_message != "Download stopped.");
        std::printf("       status=%s msg=\"%s\"\n",
                    StatusName(got.status), got.error_message.c_str());

        Cleanup(out);
        srv.Stop();
    }

    std::printf("HLS worker keep-alive connection reuse and AES-128 decryption\n");
    {
        // Keep-alive HTTP/1.1 loopback server serving an AES-128 encrypted
        // 12-segment playlist. Counts accepted TCP sockets vs served HTTP
        // requests to prove HlsDownloader::Worker reuses its CURL* handle.
        struct HlsKeepAliveServer {
            SOCKET sock = INVALID_SOCKET;
            uint16_t port = 0;
            std::thread th;
            std::atomic<int> accepted{0};
            std::atomic<int> requests{0};
            std::string key_bytes = "0123456789abcdef"; // 16 bytes
            std::vector<std::string> enc_segs;
            std::string expected_plain;

            static std::string EncryptAes128Cbc(const std::string& plain,
                                                const std::string& key,
                                                uint64_t seq) {
                uint8_t iv[16] = {0};
                for (int i = 0; i < 8; ++i)
                    iv[8 + i] = static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFF);
                BCRYPT_ALG_HANDLE hAlg = nullptr;
                BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
                BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                                  reinterpret_cast<PUCHAR>(BCRYPT_CHAIN_MODE_CBC),
                                  sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
                BCRYPT_KEY_HANDLE hKey = nullptr;
                BCryptGenerateSymmetricKey(
                    hAlg, &hKey, nullptr, 0,
                    reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), 16, 0);
                ULONG out_len = 0;
                BCryptEncrypt(hKey,
                              reinterpret_cast<PUCHAR>(const_cast<char*>(plain.data())),
                              static_cast<ULONG>(plain.size()), nullptr, iv, 16,
                              nullptr, 0, &out_len, BCRYPT_BLOCK_PADDING);
                std::string out(out_len, '\0');
                uint8_t iv2[16] = {0};
                for (int i = 0; i < 8; ++i)
                    iv2[8 + i] = static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFF);
                BCryptEncrypt(hKey,
                              reinterpret_cast<PUCHAR>(const_cast<char*>(plain.data())),
                              static_cast<ULONG>(plain.size()), nullptr, iv2, 16,
                              reinterpret_cast<PUCHAR>(out.data()), out_len,
                              &out_len, BCRYPT_BLOCK_PADDING);
                BCryptDestroyKey(hKey);
                BCryptCloseAlgorithmProvider(hAlg, 0);
                return out;
            }

            bool Start() {
                for (int i = 0; i < 12; ++i) {
                    std::string seg(1000 + i * 17, static_cast<char>('A' + (i % 26)));
                    expected_plain += seg;
                    enc_segs.push_back(EncryptAes128Cbc(seg, key_bytes, i));
                }
                sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (sock == INVALID_SOCKET) return false;
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                a.sin_port = 0;
                if (bind(sock, (sockaddr*)&a, sizeof(a)) || listen(sock, 32))
                    return false;
                int len = sizeof(a);
                getsockname(sock, (sockaddr*)&a, &len);
                port = ntohs(a.sin_port);
                th = std::thread([this] {
                    for (;;) {
                        SOCKET c = accept(sock, nullptr, nullptr);
                        if (c == INVALID_SOCKET) return;
                        ++accepted;
                        std::thread([this, c] { ServeConn(c); }).detach();
                    }
                });
                return true;
            }

            void ServeConn(SOCKET c) {
                DWORD tv = 2000;
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
                std::string buf_in;
                char tmp[4096];
                for (;;) {
                    while (buf_in.find("\r\n\r\n") == std::string::npos) {
                        int n = recv(c, tmp, sizeof(tmp), 0);
                        if (n <= 0) { closesocket(c); return; }
                        buf_in.append(tmp, n);
                    }
                    const size_t hdr_end = buf_in.find("\r\n\r\n");
                    const std::string req = buf_in.substr(0, hdr_end);
                    buf_in.erase(0, hdr_end + 4);
                    ++requests;

                    std::string body;
                    if (req.find("GET /index.m3u8 ") == 0) {
                        body = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
                               "#EXT-X-MEDIA-SEQUENCE:0\n"
                               "#EXT-X-KEY:METHOD=AES-128,URI=\"/key.bin\"\n";
                        for (int i = 0; i < 12; ++i)
                            body += "#EXTINF:2.0,\n/seg" + std::to_string(i) + ".ts\n";
                        body += "#EXT-X-ENDLIST\n";
                    } else if (req.find("GET /key.bin ") == 0) {
                        body = key_bytes;
                    } else if (req.find("GET /seg") == 0) {
                        int idx = std::atoi(req.c_str() + 8);
                        if (idx >= 0 && idx < (int)enc_segs.size())
                            body = enc_segs[idx];
                    }
                    std::string resp =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: " + std::to_string(body.size()) + "\r\n"
                        "Connection: keep-alive\r\n\r\n" + body;
                    send(c, resp.data(), (int)resp.size(), 0);
                }
            }

            void Stop() {
                if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
                if (th.joinable()) th.join();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        } hls_srv;

        if (hls_srv.Start()) {
            const std::string out = "ss_hls_out.ts";
            std::remove(out.c_str());
            odm::HlsDownloader hls;
            hls.SetPartCount(2);
            hls.Start("http://127.0.0.1:" + std::to_string(hls_srv.port) + "/index.m3u8",
                      out, "hls-ka");
            const odm::DownloadResult hr = hls.WaitForCompletion();

            std::ifstream f(out, std::ios::binary);
            std::string got_plain((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            f.close();
            std::remove(out.c_str());
            std::remove((out + ".id").c_str());

            Check("AES-128 HLS download completed",
                  hr.status == odm::DownloadStatus::Completed);
            Check("decrypted output matches plaintext across all 12 segments",
                  got_plain == hls_srv.expected_plain);
            Check("2 workers reused keep-alive sockets across 12 segments",
                  hls_srv.requests.load() >= 14 && hls_srv.accepted.load() <= 5);
            std::printf("       requests=%d accepted_sockets=%d\n",
                        hls_srv.requests.load(), hls_srv.accepted.load());
            hls_srv.Stop();
        }
    }

    curl_global_cleanup();
    WSACleanup();
    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
