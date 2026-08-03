// The yt-dlp route must recognize the Vimeo URL shapes and rewrite them to
// the player form before yt-dlp ever sees them.
//
// yt-dlp resolves a vimeo.com watch page through Vimeo's API, which
// authenticates with an OAuth client token Vimeo has since revoked: the
// resolve dies with a 401 before a single format is known, and the download
// never starts. The player embed page carries the same stream config with
// no token at all, so watch URLs are rewritten to
// https://player.vimeo.com/video/<id> up front. A watch page left
// unrewritten is invisible to the engines either way, because Resolve() and
// ListHeights() both gate on HasVideoId().
//
// Pure URL decisions; no network access.
//
// Wired into ctest as `ytdlp_url`.

#include <cstdio>
#include <string>

#include "YtDlp.h"

using odm::ytdlp::HasVideoId;
using odm::ytdlp::IsSupportedUrl;
using odm::ytdlp::NormalizePageUrl;

static int failures = 0;

static void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static void CheckEq(const char* what, const std::string& got,
                    const std::string& want) {
    if (got == want) {
        std::printf("  [PASS] %s\n", what);
        return;
    }
    std::printf("  [FAIL] %s\n", what);
    std::printf("         got:  %s\n", got.c_str());
    std::printf("         want: %s\n", want.c_str());
    ++failures;
}

int main() {
    std::printf("vimeo hosts are supported by the yt-dlp route\n");
    Check("a plain watch page is supported",
          IsSupportedUrl("https://vimeo.com/33698814"));
    Check("the player embed host is supported",
          IsSupportedUrl("https://player.vimeo.com/video/33698814"));
    Check("www.vimeo.com is supported",
          IsSupportedUrl("https://www.vimeo.com/33698814"));
    Check("a lookalike host is not",
          !IsSupportedUrl("https://notvimeo.com/33698814"));
    Check("YouTube support is unchanged",
          IsSupportedUrl("https://www.youtube.com/watch?v=jNQXAC9IVRw"));

    std::printf("every Vimeo URL shape names one video\n");
    Check("plain watch page", HasVideoId("https://vimeo.com/33698814"));
    Check("  with an unlisted hash",
          HasVideoId("https://vimeo.com/33698814/abcdef1234"));
    Check("channel page",
          HasVideoId("https://vimeo.com/channels/staffpicks/33698814"));
    Check("album page",
          HasVideoId("https://vimeo.com/album/654321/video/33698814"));
    Check("player embed",
          HasVideoId("https://player.vimeo.com/video/33698814"));
    Check("the home feed is not a video", !HasVideoId("https://vimeo.com/"));
    Check("the watch feed is not a video", !HasVideoId("https://vimeo.com/watch"));
    Check("a category is not a video",
          !HasVideoId("https://vimeo.com/categories/animation"));
    Check("YouTube still passes",
          HasVideoId("https://www.youtube.com/watch?v=jNQXAC9IVRw"));
    Check("the YouTube feed is still rejected",
          !HasVideoId("https://www.youtube.com/"));

    std::printf("watch pages are rewritten to the player form\n");
    CheckEq("plain watch page",
            NormalizePageUrl("https://vimeo.com/33698814"),
            "https://player.vimeo.com/video/33698814");
    CheckEq("  query/fragment do not survive",
            NormalizePageUrl("https://vimeo.com/33698814?fl=pl&fe=vl"),
            "https://player.vimeo.com/video/33698814");
    CheckEq("an unlisted hash rides along as ?h=",
            NormalizePageUrl("https://vimeo.com/33698814/abcdef1234"),
            "https://player.vimeo.com/video/33698814?h=abcdef1234");
    CheckEq("channel page keeps the video id",
            NormalizePageUrl("https://vimeo.com/channels/staffpicks/33698814"),
            "https://player.vimeo.com/video/33698814");
    CheckEq("album page picks the video id, not the album id",
            NormalizePageUrl("https://vimeo.com/album/654321/video/33698814"),
            "https://player.vimeo.com/video/33698814");
    CheckEq("www host normalizes too",
            NormalizePageUrl("https://www.vimeo.com/33698814"),
            "https://player.vimeo.com/video/33698814");

    std::printf("everything else passes through untouched\n");
    CheckEq("a player URL is already in shape",
            NormalizePageUrl("https://player.vimeo.com/video/33698814"),
            "https://player.vimeo.com/video/33698814");
    CheckEq("a YouTube watch URL",
            NormalizePageUrl("https://www.youtube.com/watch?v=jNQXAC9IVRw"),
            "https://www.youtube.com/watch?v=jNQXAC9IVRw");
    CheckEq("a Vimeo page with no video id",
            NormalizePageUrl("https://vimeo.com/watch"),
            "https://vimeo.com/watch");
    CheckEq("a plain media URL",
            NormalizePageUrl("https://cdn.example.com/movie.mp4"),
            "https://cdn.example.com/movie.mp4");

    std::printf("\n%s (%d failing checks)\n",
                failures ? "FAILED" : "ALL CHECKS PASSED", failures);
    return failures ? 1 : 0;
}
