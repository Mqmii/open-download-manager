#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace odm {

/// Identity marker for a finished pre-mux track file (.vtrk / .atrk).
///
/// Track paths are derived from the OUTPUT name, and two unrelated streams
/// can easily suggest the same one. "A finished track file is sitting here"
/// therefore says nothing about whether it holds the stream being downloaded
/// now — without a marker, a video track left by an earlier failed job gets
/// muxed with the current job's audio and the user receives someone else's
/// picture. The in-progress path already checks the playlist hash (LoadMeta);
/// this is the same check for the finished one.

/// Where the marker for `track_file` lives.
std::filesystem::path TrackIdPath(const std::filesystem::path& track_file);

/// Record which playlist produced `track_file`. Best-effort: a marker that
/// cannot be written just means the track is fetched again next time.
void WriteTrackId(const std::filesystem::path& track_file,
                  uint64_t playlist_hash);

/// Is `track_file` a finished track of `playlist_hash`?
///
/// True only when the file exists and is non-empty, no workspace is left
/// beside it (`.hlsmeta` / `.hlsparts`), and its marker names this playlist.
/// A track with no marker — from another job, or from a build that did not
/// write one — is unattributable and answers false, so it is re-fetched.
bool TrackDone(const std::filesystem::path& track_file, uint64_t playlist_hash);

}  // namespace odm
