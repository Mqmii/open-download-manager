#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace odm {
namespace ytdlp {

///
/// Thin wrapper around the bundled yt-dlp.exe.
///
/// YouTube is not downloadable the way every other site in this app is: the
/// media URLs are signed by a player script that changes constantly, and the
/// page never exposes a plain file. Re-implementing that in C++ means being
/// broken every few weeks, so the extraction step — and only that step — is
/// delegated to yt-dlp, which is maintained precisely against those changes.
///
/// The download itself stays ours: Resolve() returns ordinary Range-capable
/// https URLs that go straight into DashDownloader (multi-segment, resume,
/// speed limit, our own libavformat mux). YtDlpDownloader is the fallback for
/// the videos where no such URL exists.
///
/// yt-dlp.exe ships inside the release ZIP (see tools/package.ps1), so the
/// user installs nothing.
///

/// Absolute path of the bundled yt-dlp.exe, or "" when it is not there.
/// Looked up next to ODM.exe first, then in a bin/ subfolder.
std::string ExecutablePath();

inline bool Available() { return !ExecutablePath().empty(); }

/// True for the page URLs this path handles (youtube.com / youtu.be).
bool IsSupportedUrl(const std::string& url);

/// True when the URL names ONE video (/watch?v=, /shorts/, /embed/, /live/,
/// youtu.be/ID). A bare "https://www.youtube.com/" passes IsSupportedUrl but
/// points at the home feed, and must never reach the extractor.
bool HasVideoId(const std::string& url);

/// What a page URL resolves to.
struct MediaInfo {
    std::string title;          // video title, unsanitized
    std::string ext = "mp4";    // container of the merged result
    std::string video_url;      // plain https, Range-capable
    std::string audio_url;      // "" when video_url already carries sound
    double      size = 0;       // approximate total, 0 when unknown
    std::string error;          // filled on failure (user-facing)
    // True once yt-dlp has told us WHICH video this is. It separates "the
    // video exists but has no plain URL" (worth falling back to yt-dlp doing
    // the download) from "there is no video here at all" (falling back would
    // just produce a file nobody asked for).
    bool        identified = false;
};

/// Ask yt-dlp for the direct media URLs of `page_url`. Blocking (a subprocess
/// round-trip, typically 1-3 s) — call it off the UI thread. Returns false and
/// fills `out->error` when the video cannot be resolved to plain https URLs,
/// which is the caller's cue to fall back to YtDlpDownloader.
///
/// `max_height` caps the video track (1080 for "1080p"); 0 means best
/// available. The cap is "<=", not "==", so a rung that disappeared between
/// listing the qualities and starting the download degrades one step instead
/// of failing.
bool Resolve(const std::string& page_url, MediaInfo* out, int max_height = 0);

/// The distinct video heights this page offers, best first (2160, 1440, ...),
/// for the quality menu. Storyboards and audio-only rungs are left out, as are
/// formats our engines cannot range-fetch. Blocking, like Resolve().
bool ListHeights(const std::string& page_url, std::vector<int>* out);

/// Format selectors for the fallback engine, which fetches one track per run.
/// Kept next to the one Resolve() uses so the two never drift apart.
/// `max_height` behaves as in Resolve().
std::string VideoOnlyFormat(int max_height = 0);
const char* AudioOnlyFormat();

/// Run yt-dlp with `args` and hand every stdout/stderr line to `on_line`.
/// Returns the process exit code, or -1 when it could not be started. When
/// `cancel` is non-null and flips to true the process is terminated.
int Run(const std::vector<std::string>& args,
        const std::function<void(const std::string&)>& on_line,
        const std::atomic<bool>* cancel);

/// Fire-and-forget `yt-dlp -U` on a detached thread, at most once a week
/// (a stamp file next to the executable throttles it). This is what keeps
/// YouTube support working: an out-of-date yt-dlp is the single most likely
/// reason for a download to start failing months after release.
void SelfUpdateAsync();

} // namespace ytdlp
} // namespace odm
