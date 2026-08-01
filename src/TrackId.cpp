#include "TrackId.h"

#include <fstream>

namespace odm {

namespace fs = std::filesystem;

fs::path TrackIdPath(const fs::path& track_file) {
    return track_file.string() + ".id";
}

void WriteTrackId(const fs::path& track_file, uint64_t playlist_hash) {
    std::ofstream out(TrackIdPath(track_file), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(&playlist_hash),
              sizeof(playlist_hash));
}

bool TrackDone(const fs::path& track_file, uint64_t playlist_hash) {
    std::error_code ec;
    if (!fs::exists(track_file, ec) || fs::file_size(track_file, ec) == 0)
        return false;
    // A workspace beside the file means the track was still being built.
    if (fs::exists(track_file.string() + ".hlsmeta", ec)) return false;
    if (fs::exists(track_file.string() + ".hlsparts", ec)) return false;

    std::ifstream in(TrackIdPath(track_file), std::ios::binary);
    if (!in) return false;
    uint64_t got = 0;
    in.read(reinterpret_cast<char*>(&got), sizeof(got));
    return in.gcount() == static_cast<std::streamsize>(sizeof(got)) &&
           got == playlist_hash;
}

}  // namespace odm
