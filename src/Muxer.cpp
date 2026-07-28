#include "Muxer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/packet.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
}

#include <vector>

namespace odm {
namespace {

struct Input {
    AVFormatContext* ctx = nullptr;
    AVPacket*        pkt = nullptr;
    std::vector<int> map;    // input stream index -> output stream index (-1 = drop)
    bool             eof = false;
};

bool OpenInput(const std::string& path, Input& in, std::string& err) {
    if (avformat_open_input(&in.ctx, path.c_str(), nullptr, nullptr) < 0) {
        err = "Failed to open intermediate file: " + path;
        return false;
    }
    if (avformat_find_stream_info(in.ctx, nullptr) < 0) {
        err = "Failed to read stream info: " + path;
        return false;
    }
    in.pkt = av_packet_alloc();
    return in.pkt != nullptr;
}

// Advance to the next packet belonging to a mapped stream (skips data/subs).
bool NextPacket(Input& in) {
    if (in.eof) return false;
    for (;;) {
        if (av_read_frame(in.ctx, in.pkt) < 0) { in.eof = true; return false; }
        const int si = in.pkt->stream_index;
        if (si >= 0 && si < static_cast<int>(in.map.size()) && in.map[si] >= 0)
            return true;
        av_packet_unref(in.pkt);
    }
}

} // namespace

bool MuxAvTracks(const std::string& video_path,
                 const std::string& audio_path,
                 const std::string& out_path,
                 std::string* error) {
    std::string err;
    Input ins[2];
    AVFormatContext* out = nullptr;
    bool ok = false;

    do {
        if (!OpenInput(video_path, ins[0], err)) break;
        if (!OpenInput(audio_path, ins[1], err)) break;

        if (avformat_alloc_output_context2(&out, nullptr, nullptr,
                                           out_path.c_str()) < 0 || !out) {
            err = "Failed to create the output format.";
            break;
        }

        // Copy every audio/video stream of both inputs into the output.
        bool map_err = false;
        for (auto& in : ins) {
            in.map.assign(in.ctx->nb_streams, -1);
            for (unsigned i = 0; i < in.ctx->nb_streams; ++i) {
                const AVCodecParameters* par = in.ctx->streams[i]->codecpar;
                if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
                    par->codec_type != AVMEDIA_TYPE_AUDIO)
                    continue;
                AVStream* os = avformat_new_stream(out, nullptr);
                if (!os || avcodec_parameters_copy(os->codecpar, par) < 0) {
                    map_err = true;
                    break;
                }
                os->codecpar->codec_tag = 0;   // let the mp4 muxer re-tag
                os->time_base = in.ctx->streams[i]->time_base;
                in.map[i] = os->index;
            }
            if (map_err) break;
        }
        if (map_err || out->nb_streams == 0) {
            err = "Failed to copy streams (codec may be incompatible).";
            break;
        }

        if (!(out->oformat->flags & AVFMT_NOFILE) &&
            avio_open(&out->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            err = "Failed to open the output file for writing.";
            break;
        }
        // CMAF audio often carries priming samples with negative timestamps;
        // the mp4 muxer rejects them unless shifted to zero.
        out->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "movflags", "+faststart", 0);   // moov up front
        const int hrc = avformat_write_header(out, &opts);
        av_dict_free(&opts);
        if (hrc < 0) { err = "Failed to write the container header."; break; }

        // Two-way merge ordered by dts so the interleaving writer never has
        // to buffer a whole track in memory.
        bool have[2] = { NextPacket(ins[0]), NextPacket(ins[1]) };
        bool write_err = false;
        while (have[0] || have[1]) {
            int pick;
            if (have[0] && have[1]) {
                const AVRational tb0 =
                    ins[0].ctx->streams[ins[0].pkt->stream_index]->time_base;
                const AVRational tb1 =
                    ins[1].ctx->streams[ins[1].pkt->stream_index]->time_base;
                pick = av_compare_ts(ins[0].pkt->dts, tb0,
                                     ins[1].pkt->dts, tb1) <= 0 ? 0 : 1;
            } else {
                pick = have[0] ? 0 : 1;
            }
            Input& in = ins[pick];
            const AVStream* is = in.ctx->streams[in.pkt->stream_index];
            AVStream* os = out->streams[in.map[in.pkt->stream_index]];
            av_packet_rescale_ts(in.pkt, is->time_base, os->time_base);
            in.pkt->stream_index = os->index;
            in.pkt->pos = -1;
            // Takes ownership of the packet ref (blanks pkt on success).
            if (av_interleaved_write_frame(out, in.pkt) < 0) {
                write_err = true;
                break;
            }
            av_packet_unref(in.pkt);
            have[pick] = NextPacket(in);
        }
        if (write_err) { err = "Failed to write packet (disk may be full)."; break; }
        if (av_write_trailer(out) < 0) { err = "Failed to finalize the file."; break; }
        ok = true;
    } while (false);

    for (auto& in : ins) {
        if (in.pkt) av_packet_free(&in.pkt);
        if (in.ctx) avformat_close_input(&in.ctx);
    }
    if (out) {
        if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb)
            avio_closep(&out->pb);
        avformat_free_context(out);
    }
    if (!ok && error) *error = err;
    return ok;
}

} // namespace odm
