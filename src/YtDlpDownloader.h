#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "Downloader.h"   // ProgressInfo, DownloadResult

namespace odm {

///
/// Fallback engine for videos that Resolve() cannot reduce to plain https
/// URLs — YouTube increasingly serves some formats through a protocol that is
/// not a Range-capable file, and our multi-segment engine has nothing to bite
/// on there. Rather than fail, the job is handed to yt-dlp itself.
///
/// This is the slow path by design: one connection, no resume. It exists so
/// that "download this video" always has an answer, not to be fast.
///
/// ffmpeg.exe is deliberately NOT required: the two tracks are fetched
/// separately (yt-dlp is told to merge nothing) and joined by our own Muxer,
/// the same libavformat code the DASH and HLS engines use.
///
/// Public API mirrors Downloader/DashDownloader so ODMApp can route by type.
///
class YtDlpDownloader {
public:
    using ProgressCallback     = std::function<void(const ProgressInfo&)>;
    using CompletionCallback   = std::function<void(const DownloadResult&)>;
    using PathResolvedCallback =
        std::function<void(const std::string& id, const std::string& path)>;

    YtDlpDownloader() = default;
    ~YtDlpDownloader();

    YtDlpDownloader(const YtDlpDownloader&) = delete;
    YtDlpDownloader& operator=(const YtDlpDownloader&) = delete;

    /// Global speed cap in bytes/sec (0 = unlimited), passed to yt-dlp -r.
    void SetSpeedLimit(uint64_t bytes_per_sec);

    /// Cap the video track at this height (1080 for "1080p"); 0 = best.
    void SetMaxHeight(int height);

    void SetProgressCallback(ProgressCallback cb);
    void SetCompletionCallback(CompletionCallback cb);
    void SetPathResolvedCallback(PathResolvedCallback cb);

    /// Start a job for `page_url` (the watch page, not a media URL).
    /// Returns false when one is already running.
    bool Start(const std::string& page_url,
               const std::string& output_path,
               const std::string& id);

    /// Cancel the running job: the yt-dlp process is terminated and its
    /// partial track files are removed (they cannot be resumed by us).
    void Stop();

    bool IsRunning() const { return running_.load(); }

    /// Block until the worker thread has finished (used on shutdown).
    void WaitForCompletion();

private:
    void Run(std::string page_url, std::string output_path, std::string id);

    /// One yt-dlp run for one format selector. `base_bytes` is added to every
    /// progress report so the two tracks read as a single 0..100% job.
    bool FetchTrack(const std::string& page_url, const std::string& format,
                    const std::string& path, const std::string& id,
                    double base_bytes, double extra_total,
                    std::string* error);

    ProgressCallback     on_progress_;
    CompletionCallback   on_complete_;
    PathResolvedCallback on_path_;

    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::thread       thread_;

    uint64_t speed_limit_ = 0;
    int      max_height_ = 0;
};

} // namespace odm
