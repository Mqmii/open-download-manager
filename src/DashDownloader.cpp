#include "DashDownloader.h"

#include <filesystem>

#include "Muxer.h"

namespace odm {

namespace fs = std::filesystem;

DashDownloader::DashDownloader() = default;

DashDownloader::~DashDownloader() {
    Stop();
    WaitForCompletion();
}

void DashDownloader::SetPartCount(int parts) {
    if (parts < 1) parts = 1;
    if (parts > 16) parts = 16;
    part_count_ = parts;
}

void DashDownloader::SetSpeedLimit(uint64_t bytes_per_sec) {
    speed_limit_ = bytes_per_sec;
}

void DashDownloader::SetProgressCallback(ProgressCallback cb) {
    on_progress_ = std::move(cb);
}

void DashDownloader::SetCompletionCallback(CompletionCallback cb) {
    on_complete_ = std::move(cb);
}

void DashDownloader::SetPathResolvedCallback(PathResolvedCallback cb) {
    on_path_ = std::move(cb);
}

bool DashDownloader::Start(const std::string& video_url,
                           const std::string& audio_url,
                           const std::string& output_path,
                           const std::string& id,
                           const RequestContext& ctx,
                           const std::string& resume_key) {
    if (running_.exchange(true)) return false;   // one job at a time
    cancelled_.store(false);
    if (thread_.joinable()) thread_.join();      // previous run, already done
    thread_ = std::thread(&DashDownloader::Run, this, video_url, audio_url,
                          output_path, id, ctx, resume_key);
    return true;
}

void DashDownloader::Stop() {
    cancelled_.store(true);
    dl_.Stop();
}

void DashDownloader::WaitForCompletion() {
    if (thread_.joinable()) thread_.join();
}

std::string DashDownloader::GetPartInfoJson(const std::string& id) {
    return dl_.GetPartInfoJson(id);
}

bool DashDownloader::FetchTrack(const std::string& url, const std::string& path,
                                const std::string& id, const RequestContext& ctx,
                                double base_bytes, double extra_total,
                                const std::string& resume_key,
                                std::string* error) {
    // A Stop that arrived while the previous track was finishing must not be
    // answered by starting the next one: Downloader::Start clears its own stop
    // flag, so a cancel that landed before this call would be forgotten.
    if (cancelled_.load()) {
        if (error) *error = "Download stopped.";
        return false;
    }
    dl_.SetPartCount(part_count_);
    dl_.SetSpeedLimit(speed_limit_);
    // Rewrite the inner job's numbers so the two tracks read as one download.
    dl_.SetProgressCallback([this, id, base_bytes, extra_total](const ProgressInfo& p) {
        if (!on_progress_) return;
        ProgressInfo out = p;
        out.id = id;
        out.downloaded_bytes = base_bytes + p.downloaded_bytes;
        out.total_bytes = p.total_bytes > 0 ? extra_total + p.total_bytes : 0;
        on_progress_(out);
    });
    // The inner path-resolved callback is deliberately dropped: the track temp
    // is not a path the UI should ever persist.
    dl_.Start(url, path, id, ctx, resume_key);
    // ...and a Stop that landed *during* Start() was cleared by it; re-arm.
    if (cancelled_.load()) dl_.Stop();
    const DownloadResult r = dl_.WaitForCompletion();
    if (r.status != DownloadStatus::Completed) {
        if (error) *error = r.error_message;
        return false;
    }
    return true;
}

void DashDownloader::Run(std::string video_url, std::string audio_url,
                         std::string output_path, std::string id,
                         RequestContext ctx, std::string resume_key) {
    // Each track keeps its own bitmap beside its own temp file; the suffixes
    // make sure one can never be mistaken for the other. An empty key stays
    // empty so Downloader falls back to hashing the URL, as before.
    const std::string vkey = resume_key.empty() ? std::string()
                                                : resume_key + "#v";
    const std::string akey = resume_key.empty() ? std::string()
                                                : resume_key + "#a";
    DownloadResult res;
    res.id = id;
    res.file_path = output_path;
    res.status = DownloadStatus::Failed;

    const bool have_audio = !audio_url.empty();

    auto finish = [&] {
        FinalizeCancelled(res, cancelled_.load());
        if (on_path_) on_path_(id, output_path);
        running_.store(false);
        if (on_complete_) on_complete_(res);
    };

    // No audio rung? Then this is an ordinary single-file download.
    if (!have_audio) {
        std::string err;
        if (FetchTrack(video_url, output_path, id, ctx, 0, 0, vkey, &err)) {
            res.status = DownloadStatus::Completed;
        } else {
            res.error_message = err;
        }
        finish();
        return;
    }

    // Same sidecar convention as the HLS engine's CMAF path, so the resume
    // evidence next to the output pins the path across runs.
    const std::string vpath = output_path + ".vtrk";
    const std::string apath = output_path + ".atrk";

    do {
        std::string err;
        if (!FetchTrack(video_url, vpath, id, ctx, 0, 0, vkey, &err)) {
            res.error_message = "Video track download failed: " + err;
            break;
        }
        if (cancelled_.load()) break;

        double vbytes = 0;
        std::error_code ec;
        const auto vsize = fs::file_size(fs::u8path(vpath), ec);
        if (!ec) vbytes = static_cast<double>(vsize);

        if (!FetchTrack(audio_url, apath, id, ctx, vbytes, vbytes, akey, &err)) {
            res.error_message = "Audio track download failed: " + err;
            break;
        }
        if (cancelled_.load()) break;

        // Lossless remux: both tracks keep their codecs (VP9/AV1 + AAC), so
        // this is a container rewrite, not a transcode.
        std::string mux_err;
        if (!MuxAvTracks(vpath, apath, output_path, &mux_err)) {
            res.error_message = "Failed to mux audio and video: " + mux_err;
            break;
        }

        fs::remove(fs::u8path(vpath), ec);
        fs::remove(fs::u8path(apath), ec);

        const auto out_size = fs::file_size(fs::u8path(output_path), ec);
        if (!ec) {
            res.total_bytes = static_cast<double>(out_size);
            res.downloaded_bytes = res.total_bytes;
        }
        res.status = DownloadStatus::Completed;
    } while (false);

    finish();
}

} // namespace odm
