#include "FFProbeExtractor.h"
#include <filesystem>
#include <iostream>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
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
    if (!std::filesystem::exists(out.path)) {
        std::cerr << "[FFmpeg] Error: File does not exist: " << out.path << '\n';
        return false;
    }

    auto formatCtxOpt = open_format_context(out.path);
    if (!formatCtxOpt)
        return false;

    if (avformat_find_stream_info(formatCtxOpt->get(), nullptr) < 0) {
        std::cerr << "[FFmpeg] Error: Failed to find stream info for file: " << out.path << '\n';
        return false;
    }

    out.duration = static_cast<int>(formatCtxOpt->get()->duration / AV_TIME_BASE);
    out.size = static_cast<int>(std::filesystem::file_size(out.path));
    out.bit_rate = static_cast<int>(formatCtxOpt->get()->bit_rate);

    for (unsigned i = 0; i < formatCtxOpt->get()->nb_streams; ++i) {
        auto* stream = formatCtxOpt->get()->streams[i];
        if (!stream || !stream->codecpar) {
            std::cerr << "[FFmpeg] Warning: Invalid stream " << i << " in file: " << out.path << '\n';
            continue;
        }

        auto* params = stream->codecpar;
        auto const* codec = avcodec_find_decoder(params->codec_id);
        std::string codecName = codec ? codec->name : "unknown";

        if (params->codec_type == AVMEDIA_TYPE_VIDEO) {
            out.video_codec = codecName;
            out.width = params->width;
            out.height = params->height;

            auto r = stream->avg_frame_rate;
            out.avg_frame_rate = r.den > 0 ? static_cast<double>(r.num) / r.den : 0.0;

        } else if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
            out.audio_codec = codecName;
            out.sample_rate_avg = params->sample_rate;
        }
    }

    return true;
}
