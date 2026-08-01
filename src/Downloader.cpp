#include "Downloader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#endif

namespace odm {

namespace fs = std::filesystem;

std::string UniquifyPath(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return path;
    // Resume evidence beside the file: this is a CONTINUATION, keep the path
    // (renaming to "(1)" would abandon the partial data and restart fresh).
    if (fs::exists(path + ".odmprog", ec)  ||
        fs::exists(path + ".hlsmeta", ec)  ||
        fs::exists(path + ".hlsparts", ec) ||
        fs::exists(path + ".vtrk", ec)     ||
        fs::exists(path + ".vtrk.hlsmeta", ec))
        return path;
    const fs::path p(path);
    const std::string stem = p.stem().string();
    const std::string ext  = p.extension().string();
    const fs::path    dir  = p.parent_path();
    for (int i = 1; i < 1000; ++i) {
        fs::path cand = dir / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!fs::exists(cand, ec)) return cand.string();
    }
    return path;   // absurd collision count: fall back to the old behavior
}

// --- libcurl helpers -------------------------------------------------------

// Extended progress context: abort flag + live byte counter for real-time UI.
struct XferCtx {
    std::atomic<bool>*     stop;
    std::atomic<uint64_t>* live;
    curl_off_t             prev = 0;
};

static int CurlProgressXfer(void* clientp,
                            curl_off_t /*dltotal*/,
                            curl_off_t dlnow,
                            curl_off_t /*ultotal*/,
                            curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<XferCtx*>(clientp);
    if (ctx->stop->load(std::memory_order_relaxed)) return 1;
    curl_off_t delta = dlnow - ctx->prev;
    if (delta > 0) {
        ctx->live->fetch_add(static_cast<uint64_t>(delta),
                             std::memory_order_relaxed);
        ctx->prev = dlnow;
    }
    return 0;
}

// Write callback: appends received bytes to the part's open file and bumps
// a done-counter. We pass pointers via void* userdata to avoid exposing
// the private Part type here.
struct WriteCtx {
    std::atomic<uint64_t>* done_counter;
    std::fstream*         out;
};

// RAII guard that cleans up a CURL* on every exit path.
struct CurlGuard {
    CURL* c;
    ~CurlGuard() { if (c) curl_easy_cleanup(c); }
};

static size_t CurlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t bytes = size * nmemb;
    auto* ctx = static_cast<WriteCtx*>(userdata);
    ctx->out->write(ptr, bytes);
    // A failed write (disk full, I/O error) must NOT advance the counter or
    // we'd report a corrupt file as complete. Returning fewer bytes than
    // requested makes libcurl abort the transfer with a write error.
    if (!ctx->out->good()) return 0;
    *ctx->done_counter += bytes;
    return bytes;
}

// Sink write callback: discards the body (used by the probe's GET fallback).
static size_t CurlDiscard(char* /*ptr*/, size_t size, size_t nmemb, void* /*ud*/) {
    return size * nmemb;
}

// Progress callback used only to abort a probe transfer when Stop() is called.
static int CurlAbortIfStopped(void* clientp,
                              curl_off_t, curl_off_t,
                              curl_off_t, curl_off_t) {
    auto* stop = static_cast<std::atomic<bool>*>(clientp);
    return stop->load(std::memory_order_relaxed) ? 1 : 0;
}

// Header callback: capture Content-Disposition (for the real filename) and
// Content-Range (whose "/<total>" suffix gives the true size on a 206 reply).
struct HeaderCtx {
    std::string content_disposition;
    std::string content_range;
};

static size_t CurlHeader(char* buffer, size_t size, size_t nmemb, void* userdata) {
    size_t len = size * nmemb;
    auto* ctx = static_cast<HeaderCtx*>(userdata);
    std::string line(buffer, len);
    // Strip trailing CRLF.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    auto starts_with_ci = [&line](const char* key) {
        size_t kl = strlen(key);
        return line.size() > kl &&
               std::equal(key, key + kl, line.begin(),
                          [](char a, char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    };
    if (starts_with_ci("content-disposition:")) {
        ctx->content_disposition = line.substr(strlen("content-disposition:"));
    } else if (starts_with_ci("content-range:")) {
        ctx->content_range = line.substr(strlen("content-range:"));
    }
    return len;
}

// --- Downloader ------------------------------------------------------------

namespace {
    std::once_flag g_curl_init;
    // Resume sidecar (.odmprog) header identifiers. Bumping the version
    // invalidates old sidecars on upgrade.
    // v2 adds a `done_bytes` field so the legacy single-part path can resume
    // mid-file (its 1-entry bitmap alone can't express a partial byte count).
    constexpr uint32_t kSidecarMagic   = 0x314D444Fu; // 'ODM1'
    constexpr uint32_t kSidecarVersion = 2u;
}

Downloader::Downloader() {
    std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

Downloader::~Downloader() {
    Stop();
    if (orchestrator_.joinable()) orchestrator_.join();
    if (progress_thread_.joinable()) progress_thread_.join();
    if (req_headers_) {
        curl_slist_free_all(req_headers_);
        req_headers_ = nullptr;
    }
}

// --- Hand-off credentials --------------------------------------------------

namespace {

// Lower-cased host of an absolute URL ("" when it doesn't parse).
std::string UrlHost(const std::string& url) {
    CURLU* h = curl_url();
    if (!h) return {};
    std::string host;
    if (curl_url_set(h, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        char* p = nullptr;
        if (curl_url_get(h, CURLUPART_HOST, &p, 0) == CURLUE_OK && p) {
            host.assign(p);
            curl_free(p);
        }
    }
    curl_url_cleanup(h);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return host;
}

// A cookie name or value carrying a tab or a newline would split the
// tab-separated line it is written into and silently redefine the cookies
// after it. Such a cookie cannot exist per RFC 6265; drop it if it shows up.
bool CookieFieldSafe(const std::string& s) {
    return s.find_first_of("\t\r\n") == std::string::npos;
}

// One Netscape cookie-file line — the format CURLOPT_COOKIELIST parses:
//   domain <TAB> tailmatch <TAB> path <TAB> secure <TAB> expiry <TAB> name <TAB> value
std::string NetscapeCookieLine(const std::string& domain, bool tailmatch,
                               const std::string& path, bool secure,
                               long long expires, const std::string& name,
                               const std::string& value) {
    std::string dom = domain;
    // The flag column is what curl actually reads, but a tailmatch domain is
    // conventionally written with the leading dot; keep the two consistent.
    if (tailmatch && !dom.empty() && dom[0] != '.') dom.insert(dom.begin(), '.');
    std::ostringstream l;
    l << dom << '\t' << (tailmatch ? "TRUE" : "FALSE") << '\t'
      << (path.empty() ? "/" : path) << '\t' << (secure ? "TRUE" : "FALSE")
      << '\t' << expires << '\t' << name << '\t' << value;
    return l.str();
}

// Fallback for a context that carried only the "k=v; k2=v2" header: the real
// per-cookie domains are unknown, so the whole set is pinned to the host it
// was captured on plus that host's subdomains. Narrower than the browser for
// a cookie inherited from a parent domain, but it never crosses a site
// boundary — which is the property that matters here.
std::vector<std::string> ScopeCookieHeader(const std::string& header,
                                           const std::string& host) {
    std::vector<std::string> lines;
    if (host.empty()) return lines;   // unparsable URL: send nothing
    size_t pos = 0;
    while (pos <= header.size()) {
        size_t semi = header.find(';', pos);
        if (semi == std::string::npos) semi = header.size();
        std::string pair = header.substr(pos, semi - pos);
        pos = semi + 1;

        const size_t a = pair.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = pair.find_last_not_of(" \t");
        pair = pair.substr(a, b - a + 1);

        const size_t eq = pair.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string name  = pair.substr(0, eq);
        const std::string value = pair.substr(eq + 1);
        if (!CookieFieldSafe(name) || !CookieFieldSafe(value)) continue;
        lines.push_back(NetscapeCookieLine(host, true, "/", false, 0,
                                           name, value));
    }
    return lines;
}

}  // namespace

void ApplyRequestContext(CURL* curl, const RequestContext& ctx,
                         const std::string& url, curl_slist* extra_headers) {
    if (!ctx.referrer.empty())
        curl_easy_setopt(curl, CURLOPT_REFERER, ctx.referrer.c_str());
    // Applied after the default UA setopt, so a browser-supplied UA wins.
    if (!ctx.user_agent.empty())
        curl_easy_setopt(curl, CURLOPT_USERAGENT, ctx.user_agent.c_str());
    if (extra_headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, extra_headers);
    // A redirect chain is not allowed to run forever; libcurl's own default
    // is unlimited.
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    // Cookies: hand them to the engine, never to CURLOPT_COOKIE (see the
    // header). Each line carries its own domain/path/secure scope, so curl
    // re-decides what to send at every hop of a redirect.
    std::vector<std::string> lines;
    if (!ctx.cookie_jar.empty()) {
        size_t pos = 0;
        while (pos < ctx.cookie_jar.size()) {
            size_t nl = ctx.cookie_jar.find('\n', pos);
            if (nl == std::string::npos) nl = ctx.cookie_jar.size();
            std::string line = ctx.cookie_jar.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // Seven fields, and a '#' would make curl read it as a comment.
            if (line.empty() || line[0] == '#') continue;
            if (std::count(line.begin(), line.end(), '\t') != 6) continue;
            lines.push_back(std::move(line));
        }
    } else if (!ctx.cookies.empty()) {
        lines = ScopeCookieHeader(ctx.cookies, UrlHost(url));
    }
    if (lines.empty()) return;

    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");   // engine on, no file
    for (const std::string& line : lines)
        curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
}

void Downloader::ApplyRequestContext(CURL* curl, const std::string& url) const {
    odm::ApplyRequestContext(curl, req_ctx_, url, req_headers_);
}

void Downloader::SetPartCount(int parts) {
    part_count_ = std::clamp(parts, 1, 16);
}

void Downloader::SetMultiSegmentThreshold(uint64_t bytes) {
    threshold_ = bytes;
}

void Downloader::SetSpeedLimit(uint64_t bytes_per_sec) {
    speed_limit_ = bytes_per_sec;
}

void Downloader::SetProgressCallback(ProgressCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    progress_cb_ = std::move(cb);
}

void Downloader::SetCompletionCallback(CompletionCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    completion_cb_ = std::move(cb);
}

void Downloader::SetPathResolvedCallback(PathResolvedCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    path_resolved_cb_ = std::move(cb);
}

bool Downloader::IsRunning() const {
    return running_.load(std::memory_order_relaxed);
}

void Downloader::Stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
}

DownloadResult Downloader::WaitForCompletion() {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [&] { return !running_.load(std::memory_order_relaxed); });
    return result_;
}

void Downloader::Start(const std::string& url, const std::string& output_path,
                       const std::string& id, const RequestContext& ctx) {
    // Reject re-entrant starts, and join any previous run's threads before
    // replacing them (otherwise std::thread::operator= terminates the process).
    // The mutex makes the check-and-join atomic: without it, two concurrent
    // Start() calls could both observe running_ == false and race to replace
    // the same thread handles.
    std::lock_guard<std::mutex> start_lk(start_mtx_);
    if (running_.load()) return;
    if (orchestrator_.joinable()) orchestrator_.join();
    if (progress_thread_.joinable()) progress_thread_.join();

    // Swap the one-shot request context (previous run's threads are joined
    // above, so nothing can still be reading the old slist).
    if (req_headers_) {
        curl_slist_free_all(req_headers_);
        req_headers_ = nullptr;
    }
    req_ctx_ = ctx;
    for (const auto& h : ctx.extra_headers)
        req_headers_ = curl_slist_append(req_headers_, h.c_str());

    url_         = url;
    output_path_ = output_path;
    id_          = id;
    stop_requested_.store(false);
    running_.store(true);
    result_ = DownloadResult{};
    result_.id = id;
    // Reset per-run state so a failed probe can't be misreported later and so
    // a previous (chunked) run's chunk bookkeeping can't leak into this one.
    chunked_mode_   = false;
    total_          = 0;
    total_chunks_   = 0;
    chunk_size_     = 0;
    resuming_       = false;
    completed_.reset();
    claimed_.reset();
    worker_states_.reset();
    supports_range_ = false;
    any_failed_.store(false);
    downloaded_.store(0);
    live_bytes_.store(0);
    chunks_left_.store(0);
    active_workers_.store(0);

    // Capture url by value so the worker never reads the member concurrently.
    std::string url_copy = url;
    orchestrator_ = std::thread([this, url = std::move(url_copy)] {
        uint64_t total = 0;
        bool supports_range = false;

        bool ok = Probe(url, total, supports_range);

        supports_range_ = supports_range;

        if (!ok) {
            result_.status = DownloadStatus::Failed;
            result_.error_message = "Failed to probe the URL "
                                    "(check the link / network / HTTPS cert).";
        } else {
            result_.status = DownloadStatus::Completed; // overwritten below on failure
            bool chunked = (total > 0 && supports_range && total >= threshold_);
            if (chunked) LaunchChunks(url, total);
            else {
                chunked_mode_ = false;
                total_ = total;
                LaunchParts(url, total, supports_range);
            }
        }

        // Wait for worker/chunk threads.
        if (chunked_mode_) {
            for (auto& w : workers_)
                if (w.joinable()) w.join();
        } else {
            for (auto& p : parts_)
                if (p.thread.joinable()) p.thread.join();
        }

        // Wait for progress thread.
        if (progress_thread_.joinable()) progress_thread_.join();

        // Determine outcome. A genuine failure must win over the stop flag,
        // because a failing worker also raises stop_requested_ to abort peers;
        // checking stop first would misreport 404/disk-full/etc. as "Stopped".
        if (result_.status == DownloadStatus::Failed) {
            // Already finalized by a probe failure; nothing more to do.
        } else if (chunked_mode_) {
            if (any_failed_.load()) {
                result_.status = DownloadStatus::Failed;
                result_.error_message =
                    fail_reason_.empty() ? "Download failed." : fail_reason_;
            } else if (stop_requested_.load()) {
                result_.status = DownloadStatus::Stopped;
                result_.file_path = output_path_;
            } else {
                result_.status = DownloadStatus::Completed;
                result_.file_path = output_path_;
                result_.total_bytes = total;
                result_.downloaded_bytes = total;
            }
        } else {
            bool all_ok = true;
            std::string err;
            for (const auto& p : parts_) {
                if (!p.ok) { all_ok = false; err = p.error_buf; break; }
            }
            if (all_ok) {
                // The file is already the final output (segments wrote
                // directly into it), so nothing to merge.
                result_.status = DownloadStatus::Completed;
                result_.file_path = output_path_;
                result_.total_bytes = total;
                result_.downloaded_bytes = total;
            } else if (stop_requested_.load()) {
                result_.status = DownloadStatus::Stopped;
                result_.file_path = output_path_;
            } else {
                result_.status = DownloadStatus::Failed;
                result_.error_message = err.empty() ? "Download failed." : err;
            }
        }

        // On success remove the resume sidecar; keep the output file.
        // On stop/failure keep both so the download can be resumed.
        if (result_.status == DownloadStatus::Completed) {
            std::error_code ec;
            fs::remove(output_path_ + ".odmprog", ec);
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            running_.store(false);
        }
        cv_.notify_all();

        CompletionCallback cb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cb = completion_cb_;
        }
        if (cb) cb(result_);
    });
}

bool Downloader::Probe(const std::string& url,
                       uint64_t& out_total,
                       bool&     out_supports_range) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);            // HEAD
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    // Ask for a 1-byte range so we can detect range support via a 206 reply.
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (compatible; ODM/0.1)");
    // Let Stop() abort a slow probe so closing the window can't hang for 30s.
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlAbortIfStopped);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stop_requested_);
    ApplyRequestContext(curl, url);

    HeaderCtx hdr{};
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr);

    // Extract the true total from a "bytes 0-0/12345" Content-Range value.
    auto total_from_range = [](const std::string& cr) -> uint64_t {
        size_t slash = cr.find('/');
        if (slash == std::string::npos) return 0;
        size_t i = slash + 1;
        while (i < cr.size() && (cr[i] == ' ' || cr[i] == '\t')) ++i;
        if (i >= cr.size() || cr[i] == '*') return 0;   // unknown total
        uint64_t v = 0; bool any = false;
        for (; i < cr.size() && std::isdigit(static_cast<unsigned char>(cr[i])); ++i) {
            v = v * 10 + static_cast<uint64_t>(cr[i] - '0'); any = true;
        }
        return any ? v : 0;
    };

    CURLcode rc = curl_easy_perform(curl);   // HEAD (may be rejected by some CDNs)

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    // 206 = server honored the Range request → supports multipart.
    out_supports_range = (rc == CURLE_OK && http_code == 206);

    // Prefer the authoritative Content-Range total; else Content-Length,
    // ignoring the bogus "1" a 0-0 range reports.
    uint64_t total = 0;
    if (!hdr.content_range.empty()) total = total_from_range(hdr.content_range);
    if (total == 0) {
        curl_off_t cl = 0;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 1 || (cl == 1 && !out_supports_range))
            total = static_cast<uint64_t>(cl);
    }

    // If HEAD failed/errored or gave no usable size, retry with a ranged GET.
    // (S3 presigned URLs, for instance, reject HEAD but serve GET.)
    if (rc != CURLE_OK || http_code >= 400 || total == 0) {
        hdr.content_range.clear();
        curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);           // GET
        curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlDiscard);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
        CURLcode rc2 = curl_easy_perform(curl);
        long code2 = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code2);
        if (rc2 == CURLE_OK && code2 < 400) {
            http_code = code2;
            out_supports_range = (code2 == 206);
            uint64_t t2 = 0;
            if (!hdr.content_range.empty()) t2 = total_from_range(hdr.content_range);
            if (t2 == 0) {
                curl_off_t cl2 = 0;
                curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl2);
                if (code2 == 200 && cl2 > 0)      t2 = static_cast<uint64_t>(cl2);
                else if (cl2 > 1)                 t2 = static_cast<uint64_t>(cl2);
            }
            total = t2;
        } else if (rc != CURLE_OK || http_code >= 400) {
            // Neither HEAD nor GET worked: the URL is unusable.
            curl_easy_cleanup(curl);
            return false;
        }
        // else: HEAD was fine but had no size (streaming) — keep total == 0.
    }

    // A lone "1" from a 0-0 range on a range-capable server means the real size
    // is unknown, not a 1-byte file; never let that masquerade as complete.
    if (out_supports_range && total == 1) total = 0;

    out_total = total;

    // Snapshot type/disposition for extension inference downstream.
    char* ct = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    content_type_ = ct ? ct : "";
    content_disposition_ = hdr.content_disposition;

    // Fix up the output filename from the server's headers BEFORE checking
    // for resume, so .part files resolve to the final (extension-bearing) name.
    {
        fs::path p(output_path_);
        if (p.extension().string().empty()) {
            std::string server_name = FileNameFromDisposition(content_disposition_);
            if (!server_name.empty()) {
                std::string safe = SanitizeForPath(server_name);
                if (!safe.empty() && fs::path(safe).extension().string().empty())
                    safe += ExtensionForType(content_type_, content_disposition_);
                if (!safe.empty())
                    output_path_ = (p.parent_path() / safe).string();
            } else {
                std::string ext = ExtensionForType(content_type_, content_disposition_);
                if (!ext.empty()) output_path_ += ext;
            }
        }
    }

    // Tell the UI the finalized path now (before any bytes are written) so it
    // persists the corrected name; otherwise a resume after a crash/close would
    // look for the partial file under the old, extension-less path.
    {
        PathResolvedCallback pcb;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pcb = path_resolved_cb_;
        }
        if (pcb) pcb(id_, output_path_);
    }

    // NOTE: resume detection happens in LaunchChunks/LaunchParts, which
    // validate the sidecar against the actual file on disk. Probe only
    // gathers size/range/type information.

    curl_easy_cleanup(curl);
    return true;
}

std::string Downloader::FileNameFromDisposition(const std::string& content_disposition) {
    std::string disp = content_disposition;
    // Skip leading whitespace (the header value often starts with spaces).
    size_t s = disp.find_first_not_of(" \t");
    if (s != std::string::npos) disp = disp.substr(s);

    // Find "filename=" (also handle the RFC 5987 "filename*=..." form).
    size_t fpos = disp.find("filename=");
    if (fpos == std::string::npos) {
        fpos = disp.find("filename*=");
        if (fpos == std::string::npos) return "";
        // filename*=UTF-8''name.ext  -> strip encoding prefix.
        std::string fn = disp.substr(fpos + strlen("filename*="));
        size_t q = fn.find('\'');
        if (q != std::string::npos) {
            size_t q2 = fn.find('\'', q + 1);
            if (q2 != std::string::npos) fn = fn.substr(q2 + 1);
        }
        return fn;
    }
    std::string fn = disp.substr(fpos + strlen("filename="));
    // Strip surrounding quotes.
    if (!fn.empty() && (fn.front() == '"' || fn.front() == '\'')) {
        char q = fn.front();
        fn = fn.substr(1);
        size_t end = fn.find(q);
        if (end != std::string::npos) fn = fn.substr(0, end);
    }
    return fn;
}

// Make a name safe for the filesystem: drop Windows-illegal chars,
// collapse repeated underscores, trim stem, and fall back to "download.bin".
std::string Downloader::SanitizeForPath(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        bool bad = (c == '<' || c == '>' || c == ':' || c == '"' ||
                    c == '/' || c == '\\' || c == '|' || c == '?' ||
                    c == '*' || c < 32);
        out += bad ? '_' : c;
    }
    std::string cleaned;
    cleaned.reserve(out.size());
    char prev = 0;
    for (char c : out) {
        if ((c == '_' || c == ' ') && (prev == '_' || prev == ' ')) continue;
        cleaned += c;
        prev = c;
    }
    // Trim a trailing dot/space (Windows strips these and it can break paths).
    while (!cleaned.empty() &&
           (cleaned.back() == '.' || cleaned.back() == ' '))
        cleaned.pop_back();
    // Trim to a safe stem length, preserving the extension.
    const size_t max_stem = 80;
    size_t dot = cleaned.find_last_of('.');
    if (dot != std::string::npos && dot > max_stem + 16) {
        std::string stem = cleaned.substr(0, max_stem);
        std::string ext = cleaned.substr(dot);
        if (ext.size() > 16) ext = ext.substr(0, 16);
        cleaned = stem + ext;
    } else if (cleaned.size() > max_stem + 16) {
        cleaned = cleaned.substr(0, max_stem + 16);
    }
    return cleaned.empty() ? "download.bin" : cleaned;
}

std::string Downloader::ExtensionForType(const std::string& content_type,
                                         const std::string& content_disposition) {
    // Fall back to a Content-Type -> extension map.
    std::string ct = content_type;
    size_t sep = ct.find(';');
    if (sep != std::string::npos) ct = ct.substr(0, sep);
    // Trim.
    size_t a = ct.find_first_not_of(" \t");
    size_t b = ct.find_last_not_of(" \t");
    if (a != std::string::npos) ct = ct.substr(a, b - a + 1);

    static const std::pair<const char*, const char*> map[] = {
        {"application/zip",                ".zip"},
        {"application/x-zip-compressed",   ".zip"},
        {"application/x-rar-compressed",   ".rar"},
        {"application/vnd.rar",            ".rar"},
        {"application/rar",                ".rar"},
        {"application/x-7z-compressed",   ".7z"},
        {"application/pdf",                ".pdf"},
        {"application/json",               ".json"},
        {"application/xml",                ".xml"},
        {"application/octet-stream",       ""},
        {"text/plain",                     ".txt"},
        {"text/html",                      ".html"},
        {"image/png",                     ".png"},
        {"image/jpeg",                    ".jpg"},
        {"image/gif",                     ".gif"},
        {"image/webp",                    ".webp"},
        {"audio/mpeg",                    ".mp3"},
        {"audio/mp4",                     ".m4a"},
        {"video/mp4",                     ".mp4"},
        {"video/webm",                    ".webm"},
        {"video/x-matroska",              ".mkv"},
    };
    for (const auto& m : map) {
        if (ct == m.first) return m.second;
    }
    // Partial/subtype matches (e.g. "application/x-rar").
    if (ct.find("rar") != std::string::npos)   return ".rar";
    if (ct.find("zip") != std::string::npos)   return ".zip";
    if (ct.find("7z")  != std::string::npos)   return ".7z";
    return "";
}

void Downloader::Preallocate(const std::string& path, uint64_t total) {
    std::error_code ec;
    // Keep an existing file that's already the right size (resume case).
    if (fs::exists(path, ec) && fs::file_size(path, ec) == total) return;

#if defined(_WIN32)
    // Create a sparse file so the OS doesn't physically write `total` zero
    // bytes up front — essential for multi-GB downloads. The path is passed
    // through std::filesystem so Unicode handling matches the fstream calls
    // used elsewhere in this file.
    std::filesystem::path fpath(path);
    HANDLE h = CreateFileW(fpath.wstring().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD unused = 0;
        DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                        &unused, nullptr);
        LARGE_INTEGER sz;
        sz.QuadPart = static_cast<LONGLONG>(total);
        SetFilePointerEx(h, sz, nullptr, FILE_END);
        SetEndOfFile(h);
        CloseHandle(h);
        return;
    }
    // Fall through to the portable path below if the Win32 call failed.
#endif

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    if (total > 0) {
        f.seekp(static_cast<std::streamoff>(total - 1));
        if (f) f.put(0);   // extends the file to `total` bytes
    }
    f.close();
}

std::vector<uint8_t> Downloader::LoadProgress(const std::string& output_path,
                                              uint64_t& out_done) {
    std::vector<uint8_t> bm;
    out_done = 0;
    std::ifstream f(output_path + ".odmprog", std::ios::binary);
    if (!f) return bm;

    // Header: magic, version, total, chunk_size, total_chunks, url_hash,
    // done_bytes (v2).
    uint32_t magic = 0, version = 0;
    uint64_t total = 0, chunk_size = 0, chunks = 0, url_hash = 0, done = 0;
    if (!f.read(reinterpret_cast<char*>(&magic),      sizeof(magic))      ||
        !f.read(reinterpret_cast<char*>(&version),    sizeof(version))    ||
        !f.read(reinterpret_cast<char*>(&total),      sizeof(total))      ||
        !f.read(reinterpret_cast<char*>(&chunk_size), sizeof(chunk_size)) ||
        !f.read(reinterpret_cast<char*>(&chunks),     sizeof(chunks))     ||
        !f.read(reinterpret_cast<char*>(&url_hash),   sizeof(url_hash))   ||
        !f.read(reinterpret_cast<char*>(&done),       sizeof(done)))
        return bm;

    // The expected layout depends on the mode: chunked runs validate against
    // their chunk geometry, while the legacy single-part path stores one
    // aggregate "chunk" covering the whole file.
    const uint64_t want_chunk_size = chunked_mode_ ? chunk_size_ : total_;
    const uint64_t want_chunks     = chunked_mode_ ? total_chunks_ : 1;

    // Reject any sidecar that doesn't describe THIS exact job: a mismatched
    // size, chunking, or URL would map the bitmap onto the wrong bytes and
    // silently corrupt the file. This also bounds the allocation below.
    if (magic != kSidecarMagic || version != kSidecarVersion ||
        total != total_ || chunk_size != want_chunk_size ||
        chunks != want_chunks || url_hash != HashUrl(url_))
        return bm;

    if (done > total) return bm;   // impossible: stale/corrupt sidecar

    bm.resize(static_cast<size_t>(chunks));
    if (!f.read(reinterpret_cast<char*>(bm.data()),
                static_cast<std::streamsize>(chunks))) {
        bm.clear();   // truncated/short bitmap: ignore it entirely
        return bm;
    }
    out_done = done;
    return bm;
}

void Downloader::SaveAllProgress() {
    std::lock_guard<std::mutex> lk(progress_file_mtx_);
    // Chunked mode persists a per-chunk bitmap. The legacy single-part path
    // persists its byte count with one aggregate bitmap entry. Legacy
    // multi-part (no Range support) and streaming (unknown size) can't be
    // resumed, so no sidecar is written for them.
    uint64_t chunks = 0, chunk_size = 0, done = 0;
    if (chunked_mode_) {
        if (total_chunks_ == 0 || !completed_) return;
        chunks     = total_chunks_;
        chunk_size = chunk_size_;
        done       = downloaded_.load();
    } else {
        if (parts_.size() != 1 || total_ == 0) return;
        chunks     = 1;
        chunk_size = total_;
        done       = parts_[0].done.load();
    }
    std::ofstream f(output_path_ + ".odmprog",
                   std::ios::binary | std::ios::trunc);
    if (!f) return;
    uint32_t magic = kSidecarMagic, version = kSidecarVersion;
    uint64_t total = total_;
    uint64_t url_hash = HashUrl(url_);
    f.write(reinterpret_cast<const char*>(&magic),      sizeof(magic));
    f.write(reinterpret_cast<const char*>(&version),    sizeof(version));
    f.write(reinterpret_cast<const char*>(&total),      sizeof(total));
    f.write(reinterpret_cast<const char*>(&chunk_size), sizeof(chunk_size));
    f.write(reinterpret_cast<const char*>(&chunks),     sizeof(chunks));
    f.write(reinterpret_cast<const char*>(&url_hash),   sizeof(url_hash));
    f.write(reinterpret_cast<const char*>(&done),       sizeof(done));
    if (chunked_mode_) {
        for (uint64_t i = 0; i < chunks; ++i) {
            uint8_t b = completed_[i].load() ? 1 : 0;
            f.write(reinterpret_cast<const char*>(&b), 1);
        }
    } else {
        uint8_t b = (done >= total_ && total_ > 0) ? 1 : 0;
        f.write(reinterpret_cast<const char*>(&b), 1);
    }
}

// FNV-1a 64-bit: a small, stable hash used only to bind a sidecar to its URL.
uint64_t Downloader::HashUrl(const std::string& url) {
    uint64_t h = 1469598103934665603ULL;   // FNV offset basis
    for (unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ULL;             // FNV prime
    }
    return h;
}

std::string Downloader::GetPartInfoJson(const std::string& id) {
    // start_mtx_ keeps this snapshot from racing a concurrent Start()'s state
    // reset; the atomics themselves are safe to read while workers run.
    std::lock_guard<std::mutex> lk(start_mtx_);
    if (!running_.load() || id.empty() || id != id_) return "{}";

    std::ostringstream js;
    js << "{\"resume\":" << (supports_range_ ? "true" : "false");

    if (chunked_mode_ && completed_ && total_chunks_ > 0) {
        // Chunk map: 0 pending, 1 in flight, 2 completed. In-flight chunks
        // are the ones currently claimed by a worker slot.
        std::vector<char> state(static_cast<size_t>(total_chunks_), 0);
        for (uint64_t i = 0; i < total_chunks_; ++i)
            if (completed_[i].load(std::memory_order_relaxed)) state[i] = 2;
        if (worker_states_) {
            for (int w = 0; w < part_count_; ++w) {
                if (!worker_states_[w].active.load(std::memory_order_relaxed))
                    continue;
                uint64_t idx = worker_states_[w].chunk_idx.load(
                    std::memory_order_relaxed);
                if (idx < total_chunks_ && state[idx] != 2) state[idx] = 1;
            }
        }
        js << ",\"segments\":[";
        for (uint64_t i = 0; i < total_chunks_; ++i) {
            if (i) js << ',';
            js << static_cast<int>(state[i]);
        }
        js << "],\"parts\":[";
        bool first = true;
        if (worker_states_) {
            for (int w = 0; w < part_count_; ++w) {
                bool active = worker_states_[w].active.load(
                    std::memory_order_relaxed) != 0;
                uint64_t done  = worker_states_[w].chunk_done.load(
                    std::memory_order_relaxed);
                uint64_t total = worker_states_[w].chunk_size.load(
                    std::memory_order_relaxed);
                if (!first) js << ',';
                first = false;
                js << "{\"i\":" << (w + 1)
                   << ",\"status\":\"" << (active ? "Receiving Data" : "Idle")
                   << "\",\"done\":" << done
                   << ",\"total\":" << total << "}";
            }
        }
        js << "]";
    } else {
        // Legacy parts mode: one segment + one row per part.
        js << ",\"segments\":[";
        for (size_t i = 0; i < parts_.size(); ++i) {
            uint64_t done = parts_[i].done.load(std::memory_order_relaxed);
            uint64_t size = parts_[i].size;
            int s = (size > 0 && done >= size) ? 2 : (done > 0 ? 1 : 1);
            if (i) js << ',';
            js << s;
        }
        js << "],\"parts\":[";
        for (size_t i = 0; i < parts_.size(); ++i) {
            uint64_t done = parts_[i].done.load(std::memory_order_relaxed);
            uint64_t size = parts_[i].size;
            bool finished = parts_[i].finished->load();
            const char* status = (size > 0 && done >= size) ? "Completed"
                                : finished ? "Stopped" : "Receiving Data";
            if (i) js << ',';
            js << "{\"i\":" << (i + 1)
               << ",\"status\":\"" << status
               << "\",\"done\":" << done
               << ",\"total\":" << size << "}";
        }
        js << "]";
    }

    js << "}";
    return js.str();
}

void Downloader::InterruptibleSleep(int ms) {
    while (ms > 0 && !stop_requested_.load(std::memory_order_relaxed)) {
        int slice = ms < 100 ? ms : 100;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        ms -= slice;
    }
}

void Downloader::LaunchParts(const std::string& url, uint64_t total,
                             bool supports_range) {
    // Legacy single-segment path (small files / no Range support / streaming).
    parts_.clear();
    // Splitting the file only means anything if the server honors Range. A
    // server that ignores it answers every worker with 200 and the WHOLE body,
    // and each of them writes that whole body at its own offset — a file
    // almost twice the right size, filled with overlapping copies, reported as
    // finished. One connection is slower but correct.
    int n = (total == 0 || total < threshold_ || !supports_range)
                ? 1 : part_count_;

    // Single-part resume: a previous run left a pre-allocated partial file
    // (size == total, sparse) plus a sidecar recording how many bytes were
    // actually written. Only trust the byte count when BOTH are present and
    // consistent — a sidecar without its file (or a truncated/replaced file)
    // would make us skip bytes and produce a corrupt, zero-holed download.
    // Resume also requires Range support so FetchPart can continue mid-file.
    uint64_t resumed = 0;
    if (n == 1 && total > 0 && supports_range) {
        std::error_code ec;
        bool file_ok = fs::exists(output_path_, ec) &&
                       fs::file_size(output_path_, ec) == total;
        if (file_ok) {
            uint64_t done = 0;
            std::vector<uint8_t> bm = LoadProgress(output_path_, done);
            if (!bm.empty() && done > 0 && done < total)
                resumed = done;
        }
    }

    // Pre-allocate the final file once; segments write into it directly.
    // (Keeps an existing file that is already the right size — the resume
    // case above — and creates a fresh sparse file otherwise.)
    if (total > 0) Preallocate(output_path_, total);

    uint64_t base = 0;
    uint64_t each = (n > 0 && total > 0) ? (total / n) : 0;
    uint64_t remainder = (n > 0 && total > 0) ? (total % n) : 0;

    for (int i = 0; i < n; ++i) {
        Part p;
        p.offset = base;
        p.size   = each + (i == n - 1 ? remainder : 0);
        p.temp_path = output_path_;   // write straight into the final file
        parts_.push_back(std::move(p));
        base += each;
    }

    if (resumed > 0) {
        parts_[0].done.store(resumed);
        live_bytes_.store(resumed);
        downloaded_.store(resumed);
        resuming_ = true;
    }

    // Start the progress aggregator thread.
    progress_thread_ = std::thread(&Downloader::ProgressLoop, this, total);

    // Start segment threads.
    for (int i = 0; i < n; ++i) {
        parts_[i].thread =
            std::thread(&Downloader::FetchPart, this, std::cref(url),
                        std::ref(parts_[i]));
    }
}

uint64_t Downloader::ChunkSize(uint64_t idx) const {
    if (total_ == 0) return 0;                 // unbounded (streaming)
    uint64_t last = total_ - idx * chunk_size_;
    return last < chunk_size_ ? last : chunk_size_;
}

void Downloader::LaunchChunks(const std::string& url, uint64_t total) {
    chunked_mode_ = true;
    total_        = total;

    // Aim for ~part_count_ * 24 small chunks so finished workers can always
    // pull more work; this keeps every connection busy until the very end.
    uint64_t want = static_cast<uint64_t>(part_count_) * 24;
    total_chunks_ = std::max<uint64_t>(want, 16);
    chunk_size_   = (total + total_chunks_ - 1) / total_chunks_;   // ceil
    total_chunks_ = (total + chunk_size_ - 1) / chunk_size_;       // exact count
    if (total_chunks_ < 1) total_chunks_ = 1;

    // Value-initialize so every flag starts at 0. Plain `new atomic<uint8_t>[n]`
    // leaves the values indeterminate in C++17, which could make a never-
    // downloaded chunk look "completed" and leave a sparse hole in the file.
    completed_.reset(new std::atomic<uint8_t>[total_chunks_]());
    claimed_.reset(new std::atomic<uint8_t>[total_chunks_]());

    Preallocate(output_path_, total);

    // Restore completed chunks from the resume sidecar. LoadProgress only
    // returns a bitmap when its header matches this exact job, so it's either
    // empty or exactly total_chunks_ bytes long.
    uint64_t sidecar_done = 0;
    std::vector<uint8_t> bm = LoadProgress(output_path_, sidecar_done);
    uint64_t resumed = 0, resumed_chunks = 0;
    if (bm.size() >= total_chunks_) {
        for (uint64_t i = 0; i < total_chunks_; ++i) {
            if (bm[i]) {
                completed_[i].store(1);
                claimed_[i].store(1);          // done chunks are never re-claimed
                resumed += ChunkSize(i);
                ++resumed_chunks;
            }
        }
    }
    resuming_ = (resumed > 0);
    downloaded_.store(resumed);
    live_bytes_.store(resumed);
    chunks_left_.store(total_chunks_ - resumed_chunks);
    any_failed_.store(false);
    active_workers_.store(0);
    workers_.clear();

    SaveAllProgress();   // write the initial bitmap

    // Per-worker live state slots for the UI's part-info view.
    worker_states_.reset(new WorkerState[part_count_]());

    progress_thread_ = std::thread(&Downloader::ProgressLoop, this, total);

    for (int i = 0; i < part_count_; ++i)
        workers_.emplace_back(&Downloader::WorkerThread, this, url, i);
}

bool Downloader::ClaimChunk(uint64_t idx) {
    uint8_t expected = 0;
    return claimed_[idx].compare_exchange_strong(expected, 1);
}

void Downloader::WorkerThread(const std::string& url, int widx) {
    active_workers_.fetch_add(1);
    WorkerState* ws = worker_states_ ? &worker_states_[widx] : nullptr;

    std::fstream out;
    if (total_ > 0)
        out.open(output_path_,
                std::ios::binary | std::ios::in | std::ios::out);
    else
        out.open(output_path_,
                std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (fail_reason_.empty()) fail_reason_ = "Cannot open output file";
        any_failed_.store(true);
        active_workers_.fetch_sub(1);
        return;
    }

    const int kMaxAttempts = 6;
    // (windows.h defines a max() macro in this TU — keep it a plain ternary)
    const uint64_t stride = static_cast<uint64_t>(part_count_ > 0 ? part_count_ : 1);

    // Phase 0: this worker's own stride (widx, widx+N, widx+2N, ...) — the
    // connections spread across the WHOLE file like real ODM, which is what
    // makes the part-info strip fill as a patchwork instead of left→right.
    // Phase 1: the stride ran dry — steal whatever is still pending anywhere
    // so a slow connection can never leave a tail behind.
    for (int phase = 0; phase < 2 && !stop_requested_.load(); ++phase) {
        const uint64_t begin = (phase == 0) ? static_cast<uint64_t>(widx) : 0;
        const uint64_t step  = (phase == 0) ? stride : 1;
        for (uint64_t idx = begin;
             idx < total_chunks_ && !stop_requested_.load(); idx += step) {
            if (completed_[idx].load()) continue;     // resume: already done
            if (!ClaimChunk(idx)) continue;           // another worker owns it

            uint64_t off = (total_ == 0) ? 0 : idx * chunk_size_;
            uint64_t cs  = ChunkSize(idx);
            uint64_t end = (total_ == 0) ? 0 : off + cs;

            // Publish this worker's live chunk state for the UI.
            std::atomic<uint64_t> fallback_done{0};
            std::atomic<uint64_t>* done_ctr = &fallback_done;
            if (ws) {
                ws->chunk_idx.store(idx);
                ws->chunk_size.store(cs);
                ws->chunk_done.store(0);
                ws->active.store(1);
                done_ctr = &ws->chunk_done;
            }

            std::string err;
            uint64_t got = DownloadRange(url, out, off, end, kMaxAttempts, &err,
                                         done_ctr);
            if (ws) ws->active.store(0);
            if (stop_requested_.load()) { phase = 2; break; }
            if (got == 0) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (fail_reason_.empty()) fail_reason_ = err;
                any_failed_.store(true);
                stop_requested_.store(true);   // abort the other workers
                phase = 2;
                break;
            }
            completed_[idx].store(1);
            chunks_left_.fetch_sub(1);
            SaveAllProgress();
        }
    }

    out.close();
    active_workers_.fetch_sub(1);
}

bool Downloader::DownloadRange(const std::string& url, std::fstream& out,
                               uint64_t off, uint64_t end, int attempts,
                               std::string* err,
                               std::atomic<uint64_t>* done_ctr) {
    // `done_ctr` counts bytes written for this chunk; the caller owns it (a
    // per-worker slot that the UI can snapshot) and resets it before the call.
    std::atomic<uint64_t>& done = *done_ctr;
    WriteCtx ctx{&done, &out};
    XferCtx  xfer{&stop_requested_, &live_bytes_, 0};
    char ebuf[CURL_ERROR_SIZE] = {0};

    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (stop_requested_.load()) return 0;

        CURL* curl = curl_easy_init();
        if (!curl) { if (err) *err = "curl_easy_init failed"; return 0; }
        CurlGuard guard{curl};   // cleans up on every exit path

        uint64_t seg_start = off + done.load();   // resume from what we have
        xfer.prev = 0;   // reset for this curl_easy_perform call
        std::string range;
        if (end == 0) range.clear();                              // streaming
        else range = std::to_string(seg_start) + "-" +
                     std::to_string(end - 1);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressXfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xfer);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        if (end == 0) curl_easy_setopt(curl, CURLOPT_RANGE, nullptr);
        else          curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, ebuf);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // 1 KB/s
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  30L);      // for 30s
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (compatible; ODM/0.1)");
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        ApplyRequestContext(curl, url);
        // Apply the user's global speed cap, shared across the connections.
        if (speed_limit_ > 0) {
            uint64_t per = speed_limit_ /
                           static_cast<uint64_t>(part_count_ > 0 ? part_count_ : 1);
            if (per == 0) per = 1;
            curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                             static_cast<curl_off_t>(per));
        }

        out.clear();
        out.seekp(static_cast<std::streamoff>(seg_start));

        CURLcode rc = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (stop_requested_.load()) return 0;

        if (rc == CURLE_OK) {
            // A 200 carries the WHOLE file, so it is only what we asked for
            // when this request covers the whole file from byte zero. Any
            // other chunk would write the entire body at its own offset and
            // still look complete, so refuse it instead of banking corruption.
            const bool whole_file = (seg_start == 0 && off == 0 && end == total_);
            if (end != 0 && http_code != 206 && !whole_file) {
                if (err) *err = "Server ignored Range request";
                return 0;
            }
            if (end == 0) return done.load();   // stream finished
            if (done.load() >= (end - off)) return done.load();
            // Short read: retry from the corrected offset.
        } else if (err) {
            *err = std::string(ebuf);
        }

        if (attempt + 1 < attempts && !stop_requested_.load()) {
            int delay_ms = 500 * (1 << attempt);
            if (delay_ms > 8000) delay_ms = 8000;
            InterruptibleSleep(delay_ms);
        }
    }
    return 0;
}

void Downloader::FetchPart(const std::string& url, Part& part) {
    // Legacy single-segment / streaming path. Single-part runs with a known
    // size resume mid-file: LaunchParts seeds `part.done` from the resume
    // sidecar, and we continue writing at part.offset + done straight into
    // the final output file (no temporary .part files). Multi-part (no Range
    // support) and streaming (unknown size) runs always start from 0.

    // Open the shared output file. For range mode it was pre-allocated and
    // must NEVER be truncated (other segments are writing to it); for the
    // streaming (unknown-size) case it's the only writer, so trunc is fine.
    std::fstream out;
    if (part.size == 0)
        out.open(part.temp_path,
                std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    else
        out.open(part.temp_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) {
        part.ok = false;
        std::strncpy(part.error_buf, "Cannot open output file for writing",
                     CURL_ERROR_SIZE - 1);
        part.finished->store(true);
        return;
    }

    WriteCtx ctx{&part.done, &out};
    XferCtx  xfer{&stop_requested_, &live_bytes_, 0};

    const int kMaxAttempts = 6;
    bool succeeded = false;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (stop_requested_.load()) break;

        CURL* curl = curl_easy_init();
        if (!curl) {
            part.ok = false;
            std::strncpy(part.error_buf, "curl_easy_init failed",
                         CURL_ERROR_SIZE - 1);
            break;
        }
        CurlGuard guard{curl};
        part.easy = curl;

        // Continue from wherever the last (partial) attempt left off.
        uint64_t before    = part.done.load();
        uint64_t seg_start = part.offset + before;
        xfer.prev = 0;   // reset for this curl_easy_perform call
        std::string range;
        if (part.size == 0) {
            range.clear();                      // stream the whole file
        } else {
            range = std::to_string(seg_start) + "-" +
                    std::to_string(part.offset + part.size - 1);
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressXfer);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xfer);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        if (part.size == 0) curl_easy_setopt(curl, CURLOPT_RANGE, nullptr);
        else               curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, part.error_buf);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        // Abort connections that stall, instead of hanging the whole job.
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);  // 1 KB/s
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  30L);     // for 30s
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (compatible; ODM/0.1)");
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        ApplyRequestContext(curl, url);
        // Apply the user's global speed cap, shared across the connections.
        // Divide by the ACTUAL number of parts in this run, not part_count_:
        // a single-connection download (small file / no Range support) must
        // get the full limit, not 1/part_count_ of it.
        if (speed_limit_ > 0) {
            uint64_t n_parts = parts_.empty() ? 1 : parts_.size();
            uint64_t per = speed_limit_ / n_parts;
            if (per == 0) per = 1;
            curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                             static_cast<curl_off_t>(per));
        }

        // Position the file where this attempt should continue.
        out.clear();
        out.seekp(static_cast<std::streamoff>(seg_start));

        CURLcode rc = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        part.easy = nullptr;
        out.flush();

        if (stop_requested_.load()) { part.ok = false; break; }

        if (rc == CURLE_OK) {
            // Same rule as the chunk worker: a 200 is the whole file, so it
            // only answers this request when this part IS the whole file.
            // Otherwise every part would write the full body at its own offset
            // and report itself finished.
            const bool whole_file =
                (seg_start == 0 && part.offset == 0 && part.size == total_);
            if (part.size > 0 && http_code != 206 && !whole_file) {
                part.ok = false;
                std::strncpy(part.error_buf, "Server ignored Range request",
                             CURL_ERROR_SIZE - 1);
                break;
            }
            if (part.size == 0 || part.done >= part.size) {
                succeeded = true;
                break;
            }
            // Short read: the loop retries from the correct offset.
        }
        // Transient failure: fall through to the retry / backoff below.

        if (attempt + 1 < kMaxAttempts && !stop_requested_.load()) {
            // Exponential backoff: 0.5s, 1s, 2s, 4s, 8s (capped) so transient
            // network problems get progressively longer recovery windows.
            int delay_ms = 500 * (1 << attempt);
            if (delay_ms > 8000) delay_ms = 8000;
            InterruptibleSleep(delay_ms);
        }
    }

    out.close();

    if (stop_requested_.load())        part.ok = false;
    else if (succeeded)               part.ok = true;
    else if (part.size == 0)         part.ok = (part.done > 0);
    else                              part.ok = (part.done >= part.size);

    // Flush the final byte count so a stop/failure can be resumed from the
    // exact point we reached (no-op for multi-part / streaming runs).
    SaveAllProgress();

    part.finished->store(true);
}

void Downloader::ProgressLoop(uint64_t total) {
    // The callback is set once before a run and never changed mid-flight,
    // so snapshot it once instead of locking the mutex every tick.
    ProgressCallback cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cb = progress_cb_;
    }

    auto t_start = std::chrono::steady_clock::now();
    // Seed the baseline with bytes already present (resume), otherwise the
    // first sample would divide a huge resumed byte count by ~0.5s and report
    // an absurd speed (e.g. "1 GB/s").
    double last_bytes = static_cast<double>(live_bytes_.load());
    double last_speed = 0;
    auto   last_time  = t_start;
    int    ticks_since_save = 0;

    while (running_.load(std::memory_order_relaxed) &&
           !stop_requested_.load(std::memory_order_relaxed)) {
        ProgressInfo info{};
        info.id = id_;
        info.resuming = resuming_;
        double sum = 0;
        int    active = 0;

        if (chunked_mode_) {
            info.total_bytes = static_cast<double>(total_);
            sum   = static_cast<double>(live_bytes_.load());
            active = active_workers_.load();
        } else {
            info.total_bytes = static_cast<double>(total);
            sum = static_cast<double>(live_bytes_.load());
            for (const auto& p : parts_) {
                if (p.done.load() < p.size) ++active;
            }
        }
        info.downloaded_bytes = sum;
        info.active_parts = active;

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_time).count();
        if (dt >= 0.5) {
            last_speed = (sum - last_bytes) / dt;
            last_bytes = sum;
            last_time  = now;
        }
        info.speed_bps = last_speed;

        if (cb) cb(info);

        // Persist legacy single-part progress ~once a second so a stopped /
        // failed / killed run can resume near the point it reached. (Chunked
        // mode already saves on every completed chunk.) Writing the ~56-byte
        // sidecar once a second is negligible I/O.
        if (!chunked_mode_ && ++ticks_since_save >= 10) {
            ticks_since_save = 0;
            SaveAllProgress();
        }

        // Sleep ~100ms.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Break when the work is actually finished. active_workers_ alone is
        // NOT a finish signal at startup (this thread launches before the
        // workers do) — trust it only after a failure drained the pool.
        bool done = false;
        if (chunked_mode_) {
            done = (total_ > 0 && live_bytes_.load() >= total_) ||
                   (chunks_left_.load() == 0) ||
                   (any_failed_.load() && active_workers_.load() == 0);
        } else {
            done = true;
            for (const auto& p : parts_)
                if (!p.finished->load()) { done = false; break; }
        }
        if (done) break;
    }

    // Final callback with the latest totals.
    if (cb) {
        ProgressInfo info{};
        info.id = id_;
        info.total_bytes = static_cast<double>(chunked_mode_ ? total_ : total);
        info.downloaded_bytes = static_cast<double>(live_bytes_.load());
        cb(info);
    }
}

} // namespace odm