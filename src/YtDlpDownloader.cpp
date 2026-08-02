#include "YtDlpDownloader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
        // --continue, not --no-continue: with --no-part yt-dlp writes straight
        // to the target, so a partial left by a stopped run is exactly what it
        // needs to pick up from. Run() only lets a partial survive when the
        // plan file says it was fetched with this same format selection, so
        // there is nothing here for --continue to append to by mistake.
        "--no-part", "--no-mtime", "--continue", "--socket-timeout", "20",
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

std::string YtDlpDownloader::PlanPath(const std::string& output_path) {
    return output_path + ".ytdlp";
}

// What a partial on disk would have to have been fetched with to be worth
// continuing. yt-dlp appends to an existing file, so resuming across a change
// of quality would splice two different renditions into one broken output —
// the plan is what tells the two apart.
std::string YtDlpDownloader::PlanFor(int max_height) {
    return "odm-ytdlp-1\n" + std::to_string(max_height) + "\n" +
           ytdlp::VideoOnlyFormat(max_height) + "\n" +
           ytdlp::AudioOnlyFormat() + "\n";
}

std::string YtDlpDownloader::ReadPlan(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

void YtDlpDownloader::WritePlan(const std::string& path,
                                const std::string& plan) {
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (f) f.write(plan.data(), static_cast<std::streamsize>(plan.size()));
}

void YtDlpDownloader::Run(std::string page_url, std::string output_path,
                          std::string id) {
    DownloadResult res;
    res.id = id;
    res.file_path = output_path;
    res.status = DownloadStatus::Failed;

    const std::string vpath = output_path + ".vtrk";
    const std::string apath = output_path + ".atrk";
    const std::string plan_path = PlanPath(output_path);
    const std::string plan = PlanFor(max_height_);
    std::error_code ec;

    // Anything on disk that was fetched under a different plan (another
    // quality, an upgraded yt-dlp with a different selector) cannot be
    // continued into — drop it and start clean. A matching plan means the
    // partials below are ours and --continue may pick them up.
    if (ReadPlan(plan_path) != plan) {
        fs::remove(fs::u8path(vpath), ec);
        fs::remove(fs::u8path(apath), ec);
        fs::remove(fs::u8path(output_path), ec);
    }
    WritePlan(plan_path, plan);

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

    // A stop is a pause: leave the partial tracks and the plan exactly where
    // they are so the next launch continues instead of starting over, and do
    // not fall through to the last-resort attempt — the user asked for less
    // work, not more.
    if (cancelled_.load()) {
        res.error_message = err;
        FinalizeCancelled(res, true);
        if (on_path_) on_path_(id, output_path);
        running_.store(false);
        if (on_complete_) on_complete_(res);
        return;
    }

    // A genuine failure, on the other hand, says nothing about whether what
    // landed is usable, so it goes.
    if (res.status != DownloadStatus::Completed) {
        fs::remove(fs::u8path(vpath), ec);
        fs::remove(fs::u8path(apath), ec);
    }

    // Last resort: a single already-muxed rung. Lower quality, but a video
    // with sound beats an error — and this is the path that answers for the
    // uploads where no separate audio track is offered at all.
    if (res.status != DownloadStatus::Completed) {
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
        } else if (cancelled_.load()) {
            // Stopped during the fallback: same rule as above, keep the
            // partial output and the plan that describes it.
            res.error_message = err;
            FinalizeCancelled(res, true);
            if (on_path_) on_path_(id, output_path);
            running_.store(false);
            if (on_complete_) on_complete_(res);
            return;
        }
    }

    if (res.status != DownloadStatus::Completed) res.error_message = err;
    // Nothing left to continue: neither a finished job nor a failed one has a
    // partial worth keeping a plan for.
    fs::remove(fs::u8path(plan_path), ec);

    if (on_path_) on_path_(id, output_path);
    running_.store(false);
    if (on_complete_) on_complete_(res);
}

} // namespace odm
