#pragma once

#include <atomic>
#include <cstdint>
#include <curl/curl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Downloader.h"   // ProgressInfo, DownloadResult, RequestContext

namespace odm {

///
/// HLS (m3u8) downloader: fetch playlist -> pick highest-bandwidth variant
/// when master -> parse media playlist (EXT-X-KEY AES-128, EXT-X-MAP fMP4
/// init, EXT-X-MEDIA-SEQUENCE, EXT-X-ENDLIST) -> download segments in
/// parallel into <output>.hlsparts/ -> concatenate in order into .ts/.mp4.
///
/// CMAF masters (Reddit & co.) list the audio as a SEPARATE EXT-X-MEDIA
/// rendition — the video variant alone is silent. When the chosen variant
/// references an AUDIO group, both tracks are downloaded (video first, then
/// audio) into temp track files and remuxed losslessly into one .mp4 via
/// libavformat (Muxer.h).
///
/// Cookies/referrer/UA from the browser hand-off apply to every request
/// (playlist, key, segments). AES-128-CBC via Windows BCrypt. Resume: a
/// sidecar (<track>.hlsmeta) keeps the completed-segment bitmap per track.
/// Live playlists (no ENDLIST) and SAMPLE-AES/FairPlay DRM are rejected.
/// Public API mirrors Downloader so ODMApp can route by URL type.
///
class HlsDownloader {
public:
    using ProgressCallback     = std::function<void(const ProgressInfo&)>;
    using CompletionCallback   = std::function<void(const DownloadResult&)>;
    using PathResolvedCallback =
        std::function<void(const std::string& id, const std::string& path)>;

    HlsDownloader();
    ~HlsDownloader();

    HlsDownloader(const HlsDownloader&) = delete;
    HlsDownloader& operator=(const HlsDownloader&) = delete;

    /// Parallel segment connections (1..16, default 4). Segments are small;
    /// 4-6 is the sweet spot — more mostly angers CDNs.
    void SetPartCount(int parts);
    /// Global speed cap in bytes/sec (0 = unlimited); divided per connection.
    void SetSpeedLimit(uint64_t bytes_per_sec);

    void SetProgressCallback(ProgressCallback cb);
    void SetCompletionCallback(CompletionCallback cb);
    void SetPathResolvedCallback(PathResolvedCallback cb);

    /// Start an HLS job. Returns false when already running. `url` is the
    /// m3u8 (master or media); `id` is echoed to callbacks; `ctx` one-shot.
    bool Start(const std::string& url, const std::string& output_path,
               const std::string& id = std::string(),
               const RequestContext& ctx = RequestContext{});

    /// Graceful cancel: in-flight segments abort quickly; part files +
    /// sidecar stay on disk so the next Start resumes.
    void Stop();

    bool IsRunning() const { return running_.load(); }

    /// Block until the current job finishes; returns the final result.
    DownloadResult WaitForCompletion();

private:
    struct Segment {
        std::string url;
        uint64_t    seq = 0;   // EXT-X-MEDIA-SEQUENCE value (AES IV fallback)
        uint64_t    off = 0;   // EXT-X-BYTERANGE offset (valid when ranged)
        uint64_t    len = 0;   // EXT-X-BYTERANGE length
        bool        ranged = false;
    };
    struct KeyInfo {
        std::string method;    // "", "NONE", "AES-128", "SAMPLE-AES", ...
        std::string uri;
        std::string iv_hex;    // without 0x; empty => use segment seq
    };

    void Orchestrator();
    void Worker(int worker_index);
    void ProgressLoop();

    bool LoadPlaylist(const std::string& url, std::string& out_body,
                      std::string& out_final_url);
    bool ParseMediaPlaylist(const std::string& body,
                            const std::string& base_url);
    bool FetchToMemory(const std::string& url, std::vector<uint8_t>& out,
                       long* out_http_code, int worker_index,
                       const char* range = nullptr);
    bool FetchSegment(const std::string& url, std::vector<uint8_t>& out,
                      int worker_index, bool ranged = false,
                      uint64_t off = 0, uint64_t len = 0);
    bool FetchKey();
    bool DecryptSegment(std::vector<uint8_t>& data, uint64_t seq);

    void ApplyRequestContext(CURL* curl) const;

    void SaveMeta();
    bool LoadMeta();
    std::string SegPath(uint64_t idx) const;
    /// Download the CURRENTLY PARSED playlist (segments_/key_/init_url_)
    /// into `track_file`. Workspace: <track>.hlsparts + <track>.hlsmeta.
    /// True only when the track concatenated completely; Stop/Fail saves the
    /// sidecar for resume and returns false.
    bool RunParsedTrack(const std::filesystem::path& track_file);
    bool ConcatTo(const std::filesystem::path& out_file);

    void Fail(const std::string& msg);
    void Finish();

    int      part_count_ = 4;
    uint64_t speed_limit_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    // Signals the progress thread to exit (orchestrator sets it right before
    // joining). running_ can't be used: it only clears in Finish(), which
    // happens AFTER the join — using it here deadlocks.
    std::atomic<bool> progress_stop_{false};
    // Set alongside fail_reason_ so workers exit early WITHOUT turning the
    // result into "Stopped" (Stop uses stop_requested_, errors use abort_).
    std::atomic<bool> abort_{false};
    bool              resuming_ = false;

    RequestContext req_ctx_;
    curl_slist*    req_headers_ = nullptr;

    std::string url_;            // original m3u8 URL
    std::string playlist_url_;   // resolved media playlist URL (current track)
    std::string audio_url_;      // EXT-X-MEDIA audio rendition (empty = none)
    std::string output_path_;
    std::string id_;
    std::string fail_reason_;    // guarded by fail_mtx_
    std::mutex  fail_mtx_;

    // Parsed playlist.
    std::vector<Segment> segments_;
    std::string          init_url_;   // EXT-X-MAP (fMP4 init), may be empty
    uint64_t             init_off_ = 0;   // EXT-X-MAP BYTERANGE, when present
    uint64_t             init_len_ = 0;
    bool                 init_ranged_ = false;
    bool                 is_fmp4_ = false;
    KeyInfo              key_;

    // Work state.
    std::filesystem::path   parts_dir_;
    std::filesystem::path   meta_path_;
    std::atomic<uint64_t>   next_seg_{0};
    std::atomic<uint64_t>   segs_done_{0};      // current track only
    std::atomic<uint64_t>   bytes_done_{0};     // cumulative across tracks
    // Bytes completed before the current track started (progress estimates
    // must not mix the previous track's bytes into this track's average).
    uint64_t                track_base_bytes_ = 0;
    std::atomic<int>        active_workers_{0};
    std::vector<uint8_t>    completed_;         // per-segment flags (meta_mtx_)
    std::vector<uint8_t>    key_iv_;            // 16 bytes when IV attr present
    std::vector<uint8_t>    key_bytes_;         // 16 bytes when AES-128
    bool                    init_done_ = false; // fMP4 init segment on disk
    std::mutex              meta_mtx_;
    // Per-worker in-flight byte counters for live speed reporting.
    std::vector<std::unique_ptr<std::atomic<uint64_t>>> inflight_;

    std::vector<std::thread> workers_;
    std::thread              orchestrator_;
    std::thread              progress_thread_;
    std::mutex               start_mtx_;

    ProgressCallback     progress_cb_;
    CompletionCallback   completion_cb_;
    PathResolvedCallback path_resolved_cb_;
    DownloadResult       result_;
};

} // namespace odm
