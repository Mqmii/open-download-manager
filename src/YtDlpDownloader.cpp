#include "YtDlpDownloader.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <vector>

#include "Muxer.h"
#include "YtDlp.h"

namespace odm {

namespace fs = std::filesystem;

namespace {

// Machine-readable progress instead of the human "[download]  12.3% of ..."
// line, which changes shape between yt-dlp releases and is localized-looking
// enough to invite a fragile regex.
const char* kProgressTemplate =
    "download:ODMPROG %(progress.downloaded_bytes)s %(progress.total_bytes)s "
    "%(progress.total_bytes_estimate)s %(progress.speed)s";

double NumOrZero(const std::string& s) {
    if (s.empty() || s == "NA" || s == "None") return 0;
    return atof(s.c_str());
}

// A literal path becomes a yt-dlp output template, where % is the escape
// character. Windows paths rarely contain one, but "100%.mp4" is a legal
// file name and would otherwise be read as a field reference.
std::string EscapeTemplate(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        out += c;
        if (c == '%') out += '%';
    }
    return out;
}

} // namespace

YtDlpDownloader::~YtDlpDownloader() {
    Stop();
    WaitForCompletion();
}

void YtDlpDownloader::SetSpeedLimit(uint64_t bytes_per_sec) {
    speed_limit_ = bytes_per_sec;
}

void YtDlpDownloader::SetMaxHeight(int height) {
    max_height_ = height > 0 ? height : 0;
}

void YtDlpDownloader::SetProgressCallback(ProgressCallback cb) {
    on_progress_ = std::move(cb);
}

void YtDlpDownloader::SetCompletionCallback(CompletionCallback cb) {
    on_complete_ = std::move(cb);
}

void YtDlpDownloader::SetPathResolvedCallback(PathResolvedCallback cb) {
    on_path_ = std::move(cb);
}

bool YtDlpDownloader::Start(const std::string& page_url,
                            const std::string& output_path,
                            const std::string& id) {
    if (running_.exchange(true)) return false;   // one job at a time
    cancelled_.store(false);
    if (thread_.joinable()) thread_.join();      // previous run, already done
    thread_ = std::thread(&YtDlpDownloader::Run, this,
                          page_url, output_path, id);
    return true;
}

void YtDlpDownloader::Stop() {
    cancelled_.store(true);
}

void YtDlpDownloader::WaitForCompletion() {
    if (thread_.joinable()) thread_.join();
}

bool YtDlpDownloader::FetchTrack(const std::string& page_url,
                                 const std::string& format,
                                 const std::string& path,
                                 const std::string& id,
                                 double base_bytes, double extra_total,
                                 std::string* error) {
    std::vector<std::string> args = {
        "--no-warnings", "--no-playlist", "--newline", "--encoding", "utf-8",
        "--no-part", "--no-mtime", "--no-continue", "--socket-timeout", "20",
        // yt-dlp's m4a "fixup" shells out to ffmpeg, which we do not ship.
        // libavformat reads the un-fixed track fine (that is what Muxer gets
        // on the fast path too), so skip the step rather than let it fail.
        "--fixup", "never",
        "--progress-template", kProgressTemplate,
        "-f", format,
        "-o", EscapeTemplate(path),
    };
    if (speed_limit_ > 0) {
        args.push_back("-r");
        args.push_back(std::to_string(speed_limit_));
    }
    args.push_back(page_url);

    std::string err_line;
    const int code = ytdlp::Run(args, [&](const std::string& raw) {
        if (raw.rfind("ODMPROG ", 0) == 0) {
            if (!on_progress_) return;
            std::istringstream is(raw.substr(8));
            std::string done, total, est, speed;
            is >> done >> total >> est >> speed;
            const double total_b = total != "NA" && total != "None"
                                       ? NumOrZero(total) : NumOrZero(est);
            ProgressInfo p;
            p.id = id;
            p.downloaded_bytes = base_bytes + NumOrZero(done);
            p.total_bytes = total_b > 0 ? extra_total + total_b : 0;
            p.speed_bps = NumOrZero(speed);
            p.active_parts = 1;   // yt-dlp fetches with a single connection
            on_progress_(p);
            return;
        }
        if (raw.rfind("ERROR:", 0) == 0) err_line = raw;
    }, &cancelled_);

    if (code == 0) return true;
    if (error) {
        if (cancelled_.load())      *error = "Download stopped.";
        else if (code == -1)        *error = "yt-dlp.exe could not be started.";
        else if (!err_line.empty()) *error = err_line;
        else                        *error = "yt-dlp exited with code " +
                                             std::to_string(code) + ".";
    }
    return false;
}

void YtDlpDownloader::Run(std::string page_url, std::string output_path,
                          std::string id) {
    DownloadResult res;
    res.id = id;
    res.file_path = output_path;
    res.status = DownloadStatus::Failed;

    const std::string vpath = output_path + ".vtrk";
    const std::string apath = output_path + ".atrk";
    std::error_code ec;

    // Preferred route: the two tracks separately, joined by our own Muxer.
    // Letting yt-dlp merge would mean shipping ffmpeg.exe for a job the app
    // already does in-process.
    std::string err;
    bool ok = FetchTrack(page_url, ytdlp::VideoOnlyFormat(max_height_), vpath, id,
                         0, 0, &err);
    if (ok && !cancelled_.load()) {
        double vbytes = 0;
        const auto vsize = fs::file_size(fs::u8path(vpath), ec);
        if (!ec) vbytes = static_cast<double>(vsize);

        ok = FetchTrack(page_url, ytdlp::AudioOnlyFormat(), apath, id,
                        vbytes, vbytes, &err);
        if (ok) {
            std::string mux_err;
            if (MuxAvTracks(vpath, apath, output_path, &mux_err)) {
                fs::remove(fs::u8path(vpath), ec);
                fs::remove(fs::u8path(apath), ec);
                const auto out_size = fs::file_size(fs::u8path(output_path), ec);
                if (!ec) {
                    res.total_bytes = static_cast<double>(out_size);
                    res.downloaded_bytes = res.total_bytes;
                }
                res.status = DownloadStatus::Completed;
            } else {
                err = "Failed to mux audio and video: " + mux_err;
                ok = false;
            }
        }
    }

    if (res.status != DownloadStatus::Completed) {
        fs::remove(fs::u8path(vpath), ec);
        fs::remove(fs::u8path(apath), ec);
    }

    // Last resort: a single already-muxed rung. Lower quality, but a video
    // with sound beats an error — and this is the path that answers for the
    // uploads where no separate audio track is offered at all.
    if (res.status != DownloadStatus::Completed && !cancelled_.load()) {
        fs::remove(fs::u8path(output_path), ec);
        // No "+" in this selector: a merged selection would send yt-dlp
        // looking for an ffmpeg.exe we do not ship.
        const std::string cap = max_height_ > 0
            ? "[height<=" + std::to_string(max_height_) + "]" : "";
        const std::string one = "b" + cap + "[ext=mp4]/b" + cap + "/b";
        if (FetchTrack(page_url, one, output_path, id, 0, 0, &err)) {
            const auto out_size = fs::file_size(fs::u8path(output_path), ec);
            if (!ec) {
                res.total_bytes = static_cast<double>(out_size);
                res.downloaded_bytes = res.total_bytes;
            }
            res.status = DownloadStatus::Completed;
        }
    }

    if (res.status != DownloadStatus::Completed)
        res.error_message = cancelled_.load() ? "Download stopped." : err;

    if (on_path_) on_path_(id, output_path);
    running_.store(false);
    if (on_complete_) on_complete_(res);
}

} // namespace odm
