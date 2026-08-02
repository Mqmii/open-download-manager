// A resume is only a resume when the partial file is still there.
//
// The .odmprog sidecar is a bitmap of which CHUNKS of the output file are
// already on disk. LoadProgress validates the sidecar's own header — size,
// chunk geometry, URL hash — but nothing about the file those chunks live in.
// The chunked path then called Preallocate first, which recreates a missing or
// wrong-sized output as a fresh sparse file of exactly the right length, so by
// the time the bitmap was read there was no way left to tell that the data was
// gone. Every "already done" chunk was skipped and the download finished as a
// file full of zero holes, reported as Completed.
//
// That is the case below: delete the partial file, keep the sidecar, download
// again, and demand the bytes be right. The control cases make sure the fix is
// not just "never resume anything".
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
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "Downloader.h"
#include "YtDlpDownloader.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

int g_failures = 0;

void Check(const std::string& what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("  (" + detail + ")").c_str());
    if (!ok) ++g_failures;
}

// Range-honoring server that counts the body bytes it hands out, and can be
// told to trickle so a transfer can be interrupted on purpose.
class Server {
public:
    explicit Server(size_t size) : body_(size, '\0') {
        for (size_t i = 0; i < size; ++i) body_[i] = static_cast<char>(i % 251);
    }

    bool Start() {
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(sock_, (sockaddr*)&a, sizeof(a)) || listen(sock_, 64)) return false;
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
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void SetTrickle(bool on) { trickle_.store(on); }
    void ResetCounters() { body_bytes_.store(0); }
    uint64_t BodyBytes() const { return body_bytes_.load(); }

    // `query` lets the same bytes be served under a different URL, which is
    // what a re-signed CDN link amounts to.
    std::string Url(const char* query = nullptr) const {
        std::string u = "http://127.0.0.1:" + std::to_string(port_) + "/file.bin";
        if (query) { u += '?'; u += query; }
        return u;
    }
    const std::string& Body() const { return body_; }

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

        // The probe's 1-byte range is never trickled: holding it back would
        // just stall Start() inside Probe().
        const bool slow = trickle_.load() && payload.size() > 2;
        if (!head) {
            size_t sent = 0;
            while (sent < payload.size() && !closing_.load()) {
                const size_t slice = slow ? 2048 : 65536;
                const int want = (int)std::min<size_t>(slice, payload.size() - sent);
                const int n = send(c, payload.data() + sent, want, 0);
                if (n <= 0) break;
                sent += (size_t)n;
                body_bytes_.fetch_add((uint64_t)n);
                if (slow)
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
        shutdown(c, SD_SEND);
        closesocket(c);
    }

    std::string body_;
    SOCKET      sock_ = INVALID_SOCKET;
    uint16_t    port_ = 0;
    std::thread thread_;
    std::atomic<bool>     closing_{false};
    std::atomic<bool>     trickle_{false};
    std::atomic<uint64_t> body_bytes_{0};
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

// How many bytes of the file are still zero — the shape the bug produced.
size_t ZeroBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    size_t zeros = 0;
    char buf[65536];
    while (f) {
        f.read(buf, sizeof(buf));
        const std::streamsize got = f.gcount();
        for (std::streamsize i = 0; i < got; ++i)
            if (buf[i] == 0) ++zeros;
    }
    return zeros;
}

long long FileSize(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    return f ? (long long)f.tellg() : -1;
}

bool Exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return (bool)f;
}

void Cleanup(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".odmprog").c_str());
}

// Download until `ms` have passed, then Stop. Leaves the partial file and its
// sidecar on disk, which is what a real interrupted download looks like.
odm::DownloadResult Interrupt(Server& srv, const std::string& out, int ms) {
    odm::Downloader dl;
    dl.Start(srv.Url(), out, "interrupt");
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    dl.Stop();
    return dl.WaitForCompletion();
}

odm::DownloadResult RunToEnd(Server& srv, const std::string& out) {
    odm::Downloader dl;
    dl.Start(srv.Url(), out, "finish");
    return dl.WaitForCompletion();
}

// The same two, but with an explicit URL and resume key — the shape a YouTube
// job takes, where the URL differs between launches and the key does not.
odm::DownloadResult InterruptAs(const std::string& url, const std::string& out,
                                const std::string& key, int ms) {
    odm::Downloader dl;
    dl.Start(url, out, "interrupt", odm::RequestContext{}, key);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    dl.Stop();
    return dl.WaitForCompletion();
}

odm::DownloadResult RunToEndAs(const std::string& url, const std::string& out,
                               const std::string& key) {
    odm::Downloader dl;
    dl.Start(url, out, "finish", odm::RequestContext{}, key);
    return dl.WaitForCompletion();
}

}  // namespace

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Comfortably over the 1 MB multi-segment threshold, so every case below
    // takes the chunked path (the one that skipped the check).
    constexpr size_t kSize = 4 * 1024 * 1024;

    std::printf("the partial file is deleted, the sidecar survives\n");
    {
        Server srv(kSize);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ri_deleted.bin";
        Cleanup(out);

        srv.SetTrickle(true);
        const odm::DownloadResult first = Interrupt(srv, out, 900);
        Check("the first run was interrupted",
              first.status == odm::DownloadStatus::Stopped);
        Check("  it left a resume sidecar", Exists(out + ".odmprog"));
        Check("  and a preallocated partial file",
              FileSize(out) == (long long)kSize);

        // The user deletes the download in Explorer; the sidecar is a hidden
        // little file beside it and stays behind.
        std::remove(out.c_str());
        Check("  the file is gone, the sidecar is not",
              !Exists(out) && Exists(out + ".odmprog"));

        srv.SetTrickle(false);
        const odm::DownloadResult second = RunToEnd(srv, out);
        Check("the re-download completes",
              second.status == odm::DownloadStatus::Completed);
        Check("and the file is byte-for-byte correct",
              FileMatches(out, srv.Body()),
              "zeros=" + std::to_string(ZeroBytes(out)));

        Cleanup(out);
        srv.Stop();
    }

    std::printf("the partial file was replaced by something else\n");
    {
        Server srv(kSize);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ri_replaced.bin";
        Cleanup(out);

        srv.SetTrickle(true);
        Check("the first run was interrupted",
              Interrupt(srv, out, 900).status == odm::DownloadStatus::Stopped);

        // A shorter file at the same path: the sidecar's chunk map no longer
        // describes anything that is actually there.
        {
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            f << "not the download you are looking for";
        }
        Check("  the file is the wrong size now",
              FileSize(out) != (long long)kSize);

        srv.SetTrickle(false);
        const odm::DownloadResult second = RunToEnd(srv, out);
        Check("the re-download completes",
              second.status == odm::DownloadStatus::Completed);
        Check("and the file is byte-for-byte correct",
              FileMatches(out, srv.Body()),
              "zeros=" + std::to_string(ZeroBytes(out)));

        Cleanup(out);
        srv.Stop();
    }

    std::printf("a genuine resume must still skip what it already has\n");
    {
        Server srv(kSize);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ri_resume.bin";
        Cleanup(out);

        srv.SetTrickle(true);
        Check("the first run was interrupted",
              Interrupt(srv, out, 900).status == odm::DownloadStatus::Stopped);
        const uint64_t first_bytes = srv.BodyBytes();
        Check("  it fetched something", first_bytes > 0,
              std::to_string(first_bytes) + " bytes");

        // Nothing is touched on disk this time: the file and the sidecar both
        // stay, which is the case the check must NOT break.
        srv.ResetCounters();
        srv.SetTrickle(false);
        const odm::DownloadResult second = RunToEnd(srv, out);
        const uint64_t second_bytes = srv.BodyBytes();

        Check("the resume completes",
              second.status == odm::DownloadStatus::Completed);
        Check("and the file is byte-for-byte correct",
              FileMatches(out, srv.Body()),
              "zeros=" + std::to_string(ZeroBytes(out)));
        Check("the completed chunks really were skipped",
              second_bytes < (uint64_t)kSize,
              std::to_string(second_bytes) + " of " + std::to_string(kSize) +
              " bytes re-fetched");
        Check("the sidecar is cleaned up on success",
              !Exists(out + ".odmprog"));

        Cleanup(out);
        srv.Stop();
    }

    // A YouTube media link is signed and time-limited, so the app re-resolves
    // the watch page on every launch and gets a DIFFERENT url for the same
    // rung. The sidecar is bound to the url by default, which meant Resume on
    // a paused video threw away everything and started again. A caller that
    // knows a steadier identity — the watch page — passes it as the resume
    // key, and that is what the sidecar records instead.
    std::printf("the url changed between launches but the job did not\n");
    {
        Server srv(kSize);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ri_signed.bin";
        const std::string key = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
        // Two links to the same bytes, differing the way a re-signed CDN url
        // differs from the one it replaces.
        const std::string url1 = srv.Url("sig=1111&expire=1000");
        const std::string url2 = srv.Url("sig=2222&expire=2000");
        Cleanup(out);

        srv.SetTrickle(true);
        Check("the first run was interrupted",
              InterruptAs(url1, out, key, 900).status ==
                  odm::DownloadStatus::Stopped);

        srv.ResetCounters();
        srv.SetTrickle(false);
        const odm::DownloadResult second = RunToEndAs(url2, out, key);
        const uint64_t refetched = srv.BodyBytes();

        Check("the resume completes",
              second.status == odm::DownloadStatus::Completed);
        Check("and the file is byte-for-byte correct",
              FileMatches(out, srv.Body()),
              "zeros=" + std::to_string(ZeroBytes(out)));
        Check("a re-signed url still resumes what was already fetched",
              refetched < (uint64_t)kSize,
              std::to_string(refetched) + " of " + std::to_string(kSize) +
              " bytes re-fetched");

        Cleanup(out);
        srv.Stop();
    }

    // The control for the case above: without a key the url IS the identity,
    // and a changed url correctly means "a different job". This is the old
    // behaviour, and it is still what everything with a stable url relies on.
    std::printf("without a key, a changed url is a different job\n");
    {
        Server srv(kSize);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ri_nokey.bin";
        Cleanup(out);

        srv.SetTrickle(true);
        Check("the first run was interrupted",
              InterruptAs(srv.Url("sig=1111"), out, "", 900).status ==
                  odm::DownloadStatus::Stopped);

        srv.ResetCounters();
        srv.SetTrickle(false);
        const odm::DownloadResult second =
            RunToEndAs(srv.Url("sig=2222"), out, "");
        const uint64_t refetched = srv.BodyBytes();

        Check("it completes", second.status == odm::DownloadStatus::Completed);
        Check("and the file is byte-for-byte correct",
              FileMatches(out, srv.Body()));
        Check("but the whole file was fetched again",
              refetched >= (uint64_t)kSize,
              std::to_string(refetched) + " of " + std::to_string(kSize) +
              " bytes re-fetched");

        Cleanup(out);
        srv.Stop();
    }

    // The key replaces the url in the sidecar's identity; it does not replace
    // the size and chunk-geometry checks around it. A key that says "same job"
    // about content of a different length must still be refused.
    std::printf("a resume key does not override the size check\n");
    {
        const std::string out = "ri_keysize.bin";
        const std::string key = "same-key-different-content";
        Cleanup(out);

        Server small(kSize);
        if (!small.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        small.SetTrickle(true);
        Check("the first run was interrupted",
              InterruptAs(small.Url(), out, key, 900).status ==
                  odm::DownloadStatus::Stopped);
        small.Stop();

        // Same key, same path, but the content is now a different length.
        Server big(kSize + 512 * 1024);
        if (!big.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const odm::DownloadResult second = RunToEndAs(big.Url(), out, key);

        Check("the second run completes",
              second.status == odm::DownloadStatus::Completed);
        Check("and matches the NEW content exactly",
              FileMatches(out, big.Body()),
              "zeros=" + std::to_string(ZeroBytes(out)));

        Cleanup(out);
        big.Stop();
    }

    std::printf("a plain first download is unaffected\n");
    {
        Server srv(kSize);
        if (!srv.Start()) { std::printf("  [FAIL] no socket\n"); return 1; }
        const std::string out = "ri_fresh.bin";
        Cleanup(out);

        const odm::DownloadResult r = RunToEnd(srv, out);
        Check("completes", r.status == odm::DownloadStatus::Completed);
        Check("and is byte-for-byte correct", FileMatches(out, srv.Body()));

        Cleanup(out);
        srv.Stop();
    }

    // The yt-dlp fallback engine has no sidecar: it hands the whole job to
    // yt-dlp, which writes straight into the output file. Continuing that is
    // an append, so the only thing standing between a paused 1080p download
    // and a file with 720p bytes glued onto the end is the plan.
    std::printf("the yt-dlp fallback only continues its own partial\n");
    {
        using Y = odm::YtDlpDownloader;
        const std::string out = "ri_ytdlp.mp4";
        const std::string plan_path = Y::PlanPath(out);
        std::remove(plan_path.c_str());
        std::remove(out.c_str());

        Check("the plan lives beside the output",
              plan_path == out + ".ytdlp", plan_path);
        Check("the same quality plans the same way",
              Y::PlanFor(1080) == Y::PlanFor(1080));
        Check("a different quality does not",
              Y::PlanFor(1080) != Y::PlanFor(720));
        Check("and neither does best-available",
              Y::PlanFor(0) != Y::PlanFor(1080));
        Check("a plan is never empty", !Y::PlanFor(0).empty());

        Check("no plan on disk matches nothing",
              Y::ReadPlan(plan_path).empty());
        Check("  so a fresh run cannot mistake a stranger's file for its own",
              Y::ReadPlan(plan_path) != Y::PlanFor(1080));

        Y::WritePlan(plan_path, Y::PlanFor(1080));
        Check("a written plan reads back exactly",
              Y::ReadPlan(plan_path) == Y::PlanFor(1080));
        Check("  and still refuses a different quality",
              Y::ReadPlan(plan_path) != Y::PlanFor(720));

        // A partial the engine left behind must pin its own path, or the next
        // launch uniquifies to "name (1).mp4" and the partial is orphaned —
        // resumable data nobody will ever look at again.
        {
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            f << "half a video";
        }
        Check("a partial plus its plan keeps the output path",
              odm::UniquifyPath(out) == out, odm::UniquifyPath(out));

        std::remove(plan_path.c_str());
        Check("without the plan the same path is uniquified away",
              odm::UniquifyPath(out) != out, odm::UniquifyPath(out));

        std::remove(out.c_str());
    }

    curl_global_cleanup();
    WSACleanup();
    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
