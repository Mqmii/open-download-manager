#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "Downloader.h"   // ProgressInfo, DownloadResult, RequestContext

namespace odm {

///
/// Paired-track DASH downloader for CDNs that serve each representation as a
/// whole file instead of a segment list (Instagram/Facebook: one .mp4 per
/// quality rung plus a separate audio-only .mp4, both plain Range-capable
/// HTTP files).
///
/// There is no manifest to parse here — the browser extension already picked
/// the pair — so the job is just: download the video track, download the audio
/// track, remux the two losslessly into one .mp4 via libavformat (Muxer.h),
/// then delete the temp tracks. Nothing is re-encoded.
///
/// Both tracks are fetched with the regular multi-segment Downloader, so
/// part count, speed limit, resume and the browser's cookies/referrer/UA all
/// behave exactly as they do for a normal download.
///
/// Progress is reported as one job: the audio track's bytes continue where
/// the video track's left off, so the UI shows a single 0..100% run.
///
/// Public API mirrors Downloader/HlsDownloader so ODMApp can route by type.
///
class DashDownloader {
public:
    using ProgressCallback     = std::function<void(const ProgressInfo&)>;
    using CompletionCallback   = std::function<void(const DownloadResult&)>;
    using PathResolvedCallback =
        std::function<void(const std::string& id, const std::string& path)>;

    DashDownloader();
    ~DashDownloader();

    DashDownloader(const DashDownloader&) = delete;
    DashDownloader& operator=(const DashDownloader&) = delete;

    /// Parallel connections per track (1..16, default 8).
    void SetPartCount(int parts);
    /// Global speed cap in bytes/sec (0 = unlimited).
    void SetSpeedLimit(uint64_t bytes_per_sec);

    void SetProgressCallback(ProgressCallback cb);
    void SetCompletionCallback(CompletionCallback cb);
    void SetPathResolvedCallback(PathResolvedCallback cb);

    /// Start a paired job. Returns false when one is already running.
    /// `audio_url` may be empty, in which case this degrades to a plain
    /// single-file download of `video_url` (no mux step).
    bool Start(const std::string& video_url,
               const std::string& audio_url,
               const std::string& output_path,
               const std::string& id,
               const RequestContext& ctx);

    /// Cancel the running job (partial files are left for a later resume).
    void Stop();

    bool IsRunning() const { return running_.load(); }

    /// Block until the worker thread has finished (used on shutdown).
    void WaitForCompletion();

    /// Per-connection snapshot of the track being fetched right now, for the
    /// download-detail modal. Empty JSON array when idle.
    std::string GetPartInfoJson(const std::string& id);

private:
    void Run(std::string video_url, std::string audio_url,
             std::string output_path, std::string id, RequestContext ctx);

    /// Download one track to `path`, blocking until done. `base_bytes` is
    /// added to every progress report so the two tracks read as one job.
    bool FetchTrack(const std::string& url, const std::string& path,
                    const std::string& id, const RequestContext& ctx,
                    double base_bytes, double extra_total,
                    std::string* error);

    Downloader dl_;

    ProgressCallback     on_progress_;
    CompletionCallback   on_complete_;
    PathResolvedCallback on_path_;

    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::thread       thread_;

    // Set by the inner Downloader's completion callback, read by FetchTrack.
    std::mutex        result_mtx_;
    DownloadResult    last_result_;
    std::atomic<bool> track_done_{false};

    int      part_count_ = 8;
    uint64_t speed_limit_ = 0;
};

} // namespace odm
