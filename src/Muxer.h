#pragma once

#include <string>

namespace odm {

/// Remux (stream-copy, no re-encode) one video-only file and one audio-only
/// file into a single container chosen by `out_path`'s extension (.mp4).
/// CMAF/HLS split tracks (H.264 + AAC) are the intended input. Returns false
/// and fills `error` (Turkish, user-facing) when anything goes wrong; the
/// output file may be left half-written in that case — caller cleans up.
bool MuxAvTracks(const std::string& video_path,
                 const std::string& audio_path,
                 const std::string& out_path,
                 std::string* error);

} // namespace odm
