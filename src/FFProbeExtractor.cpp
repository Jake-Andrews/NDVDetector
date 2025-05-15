#include "FFProbeExtractor.h"

#include <filesystem>
#include <iostream>
#include <libavcodec/defs.h>
#include <optional>
#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

void free_format_context(AVFormatContext* ctx)
{
    if (ctx)
        avformat_close_input(&ctx);
}

std::optional<std::unique_ptr<AVFormatContext, decltype(&free_format_context)>>
open_format_context(std::string const& file_path)
{
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, file_path.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "[FFmpeg] Error: Failed to open file: " << file_path << '\n';
        return std::nullopt;
    }
    return std::unique_ptr<AVFormatContext, decltype(&free_format_context)>(ctx, free_format_context);
}

bool extract_info(VideoInfo& out)
{
    spdlog::debug("Extracting info for path: {}", out.path);

    if (!std::filesystem::exists(out.path)) {
        spdlog::error("[FFmpeg] File does not exist: {}", out.path);
        return false;
    }

    auto formatCtxOpt = open_format_context(out.path);
    if (!formatCtxOpt) {
        spdlog::error("[FFmpeg] Failed to open format context for: {}", out.path);
        return false;
    }

    if (avformat_find_stream_info(formatCtxOpt->get(), nullptr) < 0) {
        spdlog::error("[FFmpeg] Failed to find stream info for: {}", out.path);
        return false;
    }

    if (formatCtxOpt->get()->duration <= 0) {
        spdlog::error("[FFmpeg] Invalid or missing duration for file: {}", out.path);
        return false;
    }

    //  Basic container‑level properties ------------------------------------------------
    out.duration = static_cast<int>(formatCtxOpt->get()->duration / AV_TIME_BASE);
    out.size = static_cast<int64_t>(std::filesystem::file_size(out.path));
    out.bit_rate = static_cast<int>(formatCtxOpt->get()->bit_rate);

    bool has_video_stream = false;

    //  Iterate through all streams ------------------------------------------------------
    for (unsigned i = 0; i < formatCtxOpt->get()->nb_streams; ++i) {
        AVStream* stream = formatCtxOpt->get()->streams[i];
        if (!stream || !stream->codecpar) {
            spdlog::warn("[FFmpeg] Skipping invalid stream {} in file: {}", i, out.path);
            continue;
        }

        AVCodecParameters* params = stream->codecpar;
        AVCodec const* codec = avcodec_find_decoder(params->codec_id);
        std::string codecName = codec ? codec->name : "unknown";

        if (params->codec_type == AVMEDIA_TYPE_VIDEO) {
            has_video_stream = true;

            out.video_codec = codecName;
            out.width = params->width;
            out.height = params->height;

            // ---------------------- NEW: pix_fmt / profile / level ----------------------
            // Pixel‑format (e.g. "yuv420p10le")
            if (params->format != AV_PIX_FMT_NONE) {
                char const* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(params->format));
                out.pix_fmt = name ? name : "unknown";
            } else {
                out.pix_fmt = "unknown";
            }

            // Codec profile string (e.g. "High 10")
            if (params->profile != AV_FIELD_UNKNOWN && codec) {
                char const* pname = avcodec_profile_name(codec->id, params->profile);
                out.profile = pname ? pname : std::to_string(params->profile);
            } else {
                out.profile = "unknown";
            }

            // Codec level (raw integer, e.g. 40 => 4.0)
            out.level = params->level; // 0 if unspecified

            // Average frame‑rate ---------------------------------------------------------
            AVRational r = stream->avg_frame_rate;
            out.avg_frame_rate = r.den > 0 ? static_cast<double>(r.num) / r.den : 0.0;

        } else if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
            out.audio_codec = codecName;
            out.sample_rate_avg = params->sample_rate;
        }
    }

    if (!has_video_stream || out.width <= 0 || out.height <= 0) {
        spdlog::error("[FFmpeg] No valid video stream in file: {}", out.path);
        return false;
    }

    return true;
}
