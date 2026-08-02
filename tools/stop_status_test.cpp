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

    curl_global_cleanup();
    WSACleanup();
    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
