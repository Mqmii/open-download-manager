#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <curl/curl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace odm {

///
/// Progress information passed to the UI callback.
///
struct ProgressInfo {
    std::string id;               // caller-supplied download identity (for UI routing)
    double total_bytes = 0;       // 0 if the server didn't report Content-Length
    double downloaded_bytes = 0;  // bytes fetched so far, summed across parts
    double speed_bps = 0;         // rolling average bytes/second
    int    active_parts = 0;      // number of segment threads currently running
    bool   resuming = false;      // true when this run resumed from a .part file
};

///
/// Per-download HTTP request context (browser hand-off). Cookies/referrer/UA
/// and extra headers captured by the browser extension are required by many
/// servers; without them a handed-off download answers 403.
///
struct RequestContext {
    std::string referrer;
    std::string cookies;                    // "k=v; k2=v2" (Cookie header)
    // The same cookies WITH the scope the browser keeps for them: one
    // Netscape cookie-file line each ("domain\ttail\tpath\tsecure\texp\t
    // name\tvalue"), newline separated. Empty when the hand-off came from
    // something that doesn't know the scopes (see ApplyRequestContext).
    std::string cookie_jar;
    std::string user_agent;                 // overrides the default UA if set
    std::vector<std::string> extra_headers; // full "Name: value" lines
};

/// Install a hand-off context on a curl handle for a request to `url`.
///
/// Cookies go through curl's cookie engine, never CURLOPT_COOKIE. That option
/// is a fixed header: it rides along to every host in a redirect chain, so a
/// download that hops off the site it started on hands the session cookie to
/// whoever it lands on. The engine matches domain/path/secure per request
/// instead, which is what the browser does and the only thing that keeps a
/// cross-site redirect from turning into session theft.
///
/// `extra_headers` is the caller's pre-built slist (may be null).
void ApplyRequestContext(CURL* curl, const RequestContext& ctx,
                         const std::string& url, curl_slist* extra_headers);

///
/// Final status of a download.
///
enum class DownloadStatus {
    Completed,
    Stopped,
    Failed
};

struct DownloadResult {
    DownloadStatus status = DownloadStatus::Failed;
    std::string    id;            // caller-supplied download identity (for UI routing)
    std::string    file_path;     // final path on disk
    std::string    error_message; // populated on failure
    double         total_bytes = 0;
    double         downloaded_bytes = 0;
};

///
/// Multi-segment HTTP/HTTPS downloader built on libcurl.
///
/// Usage:
///   Downloader dl;
///   dl.SetProgressCallback([](const ProgressInfo& p){ ... });
///   auto fut = dl.StartAsync(url, "C:/Downloads/file.zip");
///   ...
///   dl.Stop();            // optional, cancels gracefully
///   auto result = fut.get();
///

/// Browser-style name-collision guard: when `path` already exists as a
/// finished file, returns "name (1).ext", "name (2).ext"... FRESH JOBS ONLY —
/// resume evidence next to the file (.odmprog / .hlsmeta / .hlsparts /
/// .vtrk track temps) pins the original path so partial downloads continue.
std::string UniquifyPath(const std::string& path);

class Downloader {
public:
    using ProgressCallback = std::function<void(const ProgressInfo&)>;
    using CompletionCallback = std::function<void(const DownloadResult&)>;
    // Fired once (from a worker thread) after the engine resolves the final
    // output path (e.g. after appending a server-suggested extension), so the
    // UI can persist the corrected path for later resume.
    using PathResolvedCallback =
        std::function<void(const std::string& id, const std::string& path)>;

    Downloader();
    ~Downloader();

    Downloader(const Downloader&) = delete;
    Downloader& operator=(const Downloader&) = delete;

    /// Set the number of parallel segments (1..16). Default 8.
    void SetPartCount(int parts);

    /// Set the minimum file size (in bytes) that triggers multi-segment mode.
    /// Files smaller than this are fetched with a single connection.
    /// Default 1 MB.
    void SetMultiSegmentThreshold(uint64_t bytes);

    /// Set a global download speed limit in bytes/second (0 = unlimited).
    /// The cap is divided across the active connections.
    void SetSpeedLimit(uint64_t bytes_per_sec);

    /// Optional progress callback (invoked from a worker thread ~10x/sec).
    void SetProgressCallback(ProgressCallback cb);

    /// Optional completion callback (invoked once when the job finishes).
    void SetCompletionCallback(CompletionCallback cb);

    /// Optional path-resolved callback (invoked once after the output path is
    /// finalized, before bytes are written).
    void SetPathResolvedCallback(PathResolvedCallback cb);

    /// Start a download. Returns a future-ish handle; you can also poll
    /// IsRunning() / WaitForCompletion(). Thread-safe. `id` is an opaque
    /// caller token echoed back in progress/completion callbacks.
    /// `ctx` is one-shot: it applies only to this run and is replaced on the
    /// next Start (an empty context means a plain, header-less request).
    void Start(const std::string& url, const std::string& output_path,
               const std::string& id = std::string(),
               const RequestContext& ctx = RequestContext{});

    /// Request a graceful cancellation. Segment threads will finish their
    /// current write and the partial file is kept on disk for resume.
    void Stop();

    bool IsRunning() const;

    /// Block until the current job finishes. Returns the result.
    DownloadResult WaitForCompletion();

    /// JSON snapshot of the running job's per-part state for the UI's
    /// download-detail view. Returns "{}" when idle or when `id` is not the
    /// current job. Shape:
    ///   {"resume":bool,
    ///    "segments":[0|1|2,...],          // 0 pending, 1 in flight, 2 done
    ///    "parts":[{"i":n,"status":"...","done":bytes,"total":bytes},...]}
    std::string GetPartInfoJson(const std::string& id);

    /// Make a name safe for the filesystem (drop Windows-illegal chars, etc.).
    static std::string SanitizeForPath(const std::string& raw);

private:
    struct Part {
        uint64_t offset = 0;       // byte offset within the file
        uint64_t size   = 0;       // number of bytes this part should fetch
        std::atomic<uint64_t> done{0}; // bytes fetched so far for this part
        std::string temp_path;     // the output file (segments write into it directly)
        std::thread thread;
        CURL*      easy = nullptr;
        char       error_buf[CURL_ERROR_SIZE] = {};
        std::atomic<bool> ok{false};
        // Set when FetchPart returns (shared_ptr keeps Part copyable/movable).
        std::shared_ptr<std::atomic<bool>> finished =
            std::make_shared<std::atomic<bool>>(false);

        // std::atomic is not movable, so provide move ops that transfer the
        // current value. (The thread is never live at move time.)
        Part() = default;
        Part(Part&& o) noexcept
            : offset(o.offset), size(o.size), done(o.done.load()),
            temp_path(std::move(o.temp_path)),
            easy(o.easy), ok(o.ok.load()),
              finished(std::move(o.finished)) {
            std::memcpy(error_buf, o.error_buf, CURL_ERROR_SIZE);
            o.easy = nullptr;
        }
        Part& operator=(Part&& o) noexcept {
            if (this != &o) {
                offset = o.offset;
                size   = o.size;
                done   = o.done.load();
                temp_path = std::move(o.temp_path);
                easy  = o.easy; o.easy = nullptr;
                std::memcpy(error_buf, o.error_buf, CURL_ERROR_SIZE);
                ok = o.ok.load();
                finished = std::move(o.finished);
            }
            return *this;
        }
        Part(const Part&) = delete;
        Part& operator=(const Part&) = delete;
    };

    // Phase 1: probe the URL with a HEAD request to get size + range support.
    bool Probe(const std::string& url,
               uint64_t& out_total,
               bool&     out_supports_range);

    // Map a Content-Type / Content-Disposition to a file extension (e.g.
    // ".zip", ".rar"), or "" if unknown.
    static std::string ExtensionForType(const std::string& content_type,
                                       const std::string& content_disposition);

    // Extract the server-suggested filename from a Content-Disposition
    // header value (handles both filename="..." and filename*=UTF-8''...).
    static std::string FileNameFromDisposition(const std::string& content_disposition);

    // Phase 2: launch segment threads. `supports_range` tells the legacy
    // single-part path whether a partial file on disk may be resumed (a
    // server without Range support cannot continue a partial transfer).
    void LaunchParts(const std::string& url, uint64_t total,
                     bool supports_range);

    // Dynamic-chunking download: many small chunks pulled from a shared
    // cursor by a fixed pool of worker connections (keeps all connections
    // busy until the end, avoiding the multi-segment "tail" slowdown).
    void LaunchChunks(const std::string& url, uint64_t total);
    void WorkerThread(const std::string& url, int widx);
    bool DownloadRange(const std::string& url, std::fstream& out,
                       uint64_t off, uint64_t end, int attempts,
                       std::string* err, std::atomic<uint64_t>* done_ctr);
    uint64_t ChunkSize(uint64_t idx) const;

    // Per-thread curl worker (legacy single-segment path).
    void FetchPart(const std::string& url, Part& part);

    // Aggregate progress and drive the user callback at a steady rate.
    void ProgressLoop(uint64_t total);

    // Pre-allocate the output file to `total` bytes (sparse) so each segment
    // can write into it directly at its own offset.
    void Preallocate(const std::string& path, uint64_t total);

    // Resume-state sidecar: a validated header + bitmap of completed chunks.
    // LoadProgress returns the bitmap only when the sidecar header matches the
    // current run (magic, total size, chunk size, chunk count, URL); otherwise
    // an empty vector is returned so a stale/mismatched sidecar is ignored.
    // `out_done` receives the persisted byte count (used by the legacy
    // single-part path, where the bitmap is just one aggregate flag).
    std::vector<uint8_t> LoadProgress(const std::string& output_path,
                                      uint64_t& out_done);
    void SaveAllProgress();

    // Sleep up to `ms`, returning early if a stop was requested.
    void InterruptibleSleep(int ms);

    // Stable 64-bit hash of the URL, stored in the sidecar for validation.
    static uint64_t HashUrl(const std::string& url);

    // Apply the per-run request context (referrer/cookies/UA/extra headers)
    // to a curl handle. Called for every easy handle: probe, chunk workers,
    // and legacy part workers. `url` is the request's own URL — cookies are
    // scoped against it, so it must be the one set on the handle.
    void ApplyRequestContext(CURL* curl, const std::string& url) const;

    // Claim exclusive ownership of a chunk (exactly one worker wins).
    bool ClaimChunk(uint64_t idx);

    int  part_count_ = 8;
    uint64_t threshold_ = 1 * 1024 * 1024; // 1 MB
    uint64_t speed_limit_ = 0;             // bytes/sec across all connections (0 = off)

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    // True while the current run is continuing a partial file from a previous
    // session (chunked bitmap or legacy byte-count resume). Surfaced to the
    // UI via ProgressInfo::resuming.
    bool              resuming_ = false;

    // Dynamic-chunk state. Work distribution is ODM-style STRIDED: worker w
    // owns chunks w, w+N, w+2N... so the connections fill DIFFERENT regions
    // of the file at once (the classic patchwork part-info strip); a worker
    // whose stride runs dry steals any remaining pending chunk, which keeps
    // the old shared-cursor "no slow tail" property.
    bool                  chunked_mode_ = false;
    uint64_t              total_ = 0;       // total bytes (0 = streaming)
    uint64_t              chunk_size_ = 0;
    uint64_t              total_chunks_ = 0;
    std::atomic<uint64_t> chunks_left_{0};  // chunks not yet completed
    std::atomic<uint64_t> downloaded_{0};   // total bytes written (completed chunks)
    std::atomic<uint64_t> live_bytes_{0};   // real-time bytes (in-flight + completed)
    std::atomic<int>      active_workers_{0};
    std::atomic<bool>     any_failed_{false};
    std::unique_ptr<std::atomic<uint8_t>[]> completed_; // per-chunk done flags
    std::unique_ptr<std::atomic<uint8_t>[]> claimed_;   // per-chunk claim flags
    std::vector<std::thread> workers_;
    std::string           fail_reason_;

    // Live per-worker chunk state, published for the UI's part-info view
    // (chunked mode only; legacy mode reads parts_ directly).
    struct WorkerState {
        std::atomic<uint64_t> chunk_idx{~0ull};
        std::atomic<uint64_t> chunk_done{0};
        std::atomic<uint64_t> chunk_size{0};
        std::atomic<uint8_t>  active{0};
    };
    std::unique_ptr<WorkerState[]> worker_states_;
    // From the probe; surfaced to the UI as "Resume Support".
    bool              supports_range_ = false;

    std::vector<Part> parts_;
    std::string       url_;
    std::string       output_path_;
    std::string       id_;                 // caller token echoed to UI callbacks
    std::string       content_type_;       // from HEAD response
    std::string       content_disposition_; // from HEAD response
    // One-shot request context for the current run. Written in Start() before
    // the orchestrator thread is spawned; read-only afterwards, so worker
    // threads can use it without synchronization. The slist is owned by us
    // and freed on the next Start / at destruction (it must outlive every
    // easy handle that used it).
    RequestContext    req_ctx_;
    curl_slist*       req_headers_ = nullptr;
    std::thread       orchestrator_;
    std::thread       progress_thread_;
    // Serializes Start(): the running_ check + thread join must be atomic
    // with respect to other Start() callers, or two racing calls could both
    // pass the check and one would replace live std::thread objects.
    std::mutex        start_mtx_;

    ProgressCallback     progress_cb_;
    CompletionCallback   completion_cb_;
    PathResolvedCallback path_resolved_cb_;

    mutable std::mutex  progress_file_mtx_;  // guards the resume sidecar

    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    DownloadResult          result_;
};

} // namespace odm