// When a leftover pre-mux track may be reused.
//
// A paired-track stream downloads video and audio into "<output>.vtrk" and
// "<output>.atrk" and muxes them at the end. Those names come from the OUTPUT
// file, which two unrelated streams can easily share — so a video track left
// behind by an earlier failed job sits exactly where the next job looks for
// its own. Reusing it means muxing one video with another's audio and handing
// the user a file that plays someone else's picture, with no error anywhere.
//
// Build: enable ODM_BUILD_TESTS in CMake, then `ctest -C Release`.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "TrackId.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(const std::string& what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

constexpr uint64_t kPlaylistA = 0x1111222233334444ull;
constexpr uint64_t kPlaylistB = 0xAAAABBBBCCCCDDDDull;

void Write(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// A scratch directory that cleans up after itself.
struct Scratch {
    fs::path dir;
    Scratch() {
        dir = fs::temp_directory_path() / "odm_trackid_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path operator/(const char* name) const { return dir / name; }
};

}  // namespace

int main() {
    Scratch scratch;

    std::printf("a track this job finished\n");
    {
        const fs::path t = scratch / "a.mp4.vtrk";
        Write(t, "video bytes");
        odm::WriteTrackId(t, kPlaylistA);
        Check("is reused", odm::TrackDone(t, kPlaylistA));
    }

    std::printf("a track some OTHER stream left behind\n");
    {
        const fs::path t = scratch / "b.mp4.vtrk";
        Write(t, "someone else's video");
        odm::WriteTrackId(t, kPlaylistA);
        Check("is NOT reused for a different playlist",
              !odm::TrackDone(t, kPlaylistB));
    }

    std::printf("a track with no marker at all\n");
    {
        // What an older build leaves, or a file a user dropped there.
        const fs::path t = scratch / "c.mp4.vtrk";
        Write(t, "unattributable bytes");
        Check("is NOT reused", !odm::TrackDone(t, kPlaylistA));
    }

    std::printf("a track that was still being built\n");
    {
        const fs::path t = scratch / "d.mp4.vtrk";
        Write(t, "partial");
        odm::WriteTrackId(t, kPlaylistA);
        Write(scratch / "d.mp4.vtrk.hlsmeta", "resume state");
        Check("is NOT reused while its .hlsmeta is there",
              !odm::TrackDone(t, kPlaylistA));

        std::error_code ec;
        fs::remove(scratch / "d.mp4.vtrk.hlsmeta", ec);
        fs::create_directories(scratch / "d.mp4.vtrk.hlsparts", ec);
        Check("is NOT reused while its .hlsparts directory is there",
              !odm::TrackDone(t, kPlaylistA));
        fs::remove_all(scratch / "d.mp4.vtrk.hlsparts", ec);
        Check("is reused once the workspace is gone",
              odm::TrackDone(t, kPlaylistA));
    }

    std::printf("degenerate cases\n");
    {
        Check("a missing track is not done",
              !odm::TrackDone(scratch / "nope.mp4.vtrk", kPlaylistA));

        const fs::path empty = scratch / "e.mp4.vtrk";
        Write(empty, "");
        odm::WriteTrackId(empty, kPlaylistA);
        Check("an empty track file is not done",
              !odm::TrackDone(empty, kPlaylistA));

        const fs::path trunc = scratch / "f.mp4.vtrk";
        Write(trunc, "video bytes");
        Write(odm::TrackIdPath(trunc), "abc");   // shorter than a hash
        Check("a truncated marker is not trusted",
              !odm::TrackDone(trunc, kPlaylistA));
    }

    std::printf("marker naming\n");
    {
        const fs::path t = "x/y/out.mp4.vtrk";
        Check("the marker sits beside its track",
              odm::TrackIdPath(t).string() == "x/y/out.mp4.vtrk.id" ||
              odm::TrackIdPath(t).string() == "x\\y\\out.mp4.vtrk.id");
    }

    std::printf("\n%s (%d failing checks)\n",
                g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
