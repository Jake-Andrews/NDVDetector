#include "VideoInfo.h"
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
open_format_context(std::filesystem::path const& filePath)
{
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, filePath.string().c_str(), nullptr, nullptr) != 0) {
        std::cerr << "[FFmpeg] Error: Failed to open file: " << filePath << '\n';
        return std::nullopt;
    }
    return std::unique_ptr<AVFormatContext, decltype(&free_format_context)>(ctx, free_format_context);
}

std::optional<FFProbeOutput> extract_info(std::string_view inputFile)
{
    auto const fsPath = std::filesystem::path(inputFile);
    if (!std::filesystem::exists(fsPath)) {
        std::cerr << "[FFmpeg] Error: File does not exist: " << fsPath << '\n';
        return std::nullopt;
    }

    auto formatCtxOpt = open_format_context(fsPath);
    if (!formatCtxOpt)
        return std::nullopt;

    if (avformat_find_stream_info(formatCtxOpt->get(), nullptr) < 0) {
        std::cerr << "[FFmpeg] Error: Failed to find stream info for file: " << fsPath << '\n';
        return std::nullopt;
    }

    FFProbeOutput result;
    result.format.duration = std::to_string(formatCtxOpt->get()->duration / AV_TIME_BASE);
    result.format.size = std::to_string(std::filesystem::file_size(fsPath));
    result.format.bitRate = std::to_string(formatCtxOpt->get()->bit_rate);

    for (unsigned i = 0; i < formatCtxOpt->get()->nb_streams; ++i) {

        auto* stream = formatCtxOpt->get()->streams[i];
        if (!stream || !stream->codecpar) {
            std::cerr << "[FFmpeg] Warning: Invalid stream " << i << " in file: " << fsPath << '\n';
            continue;
        }

        auto* params = stream->codecpar;
        auto const* codec = avcodec_find_decoder(params->codec_id);

        StreamInfo info;
        if (params->codec_type == AVMEDIA_TYPE_VIDEO) {
            info.codecType = "video";
            info.width = params->width;
            info.height = params->height;
            auto r = stream->avg_frame_rate;
            info.avgFrameRate.numerator = r.num;
            info.avgFrameRate.denominator = r.den;

        } else if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
            info.codecType = "audio";
            info.sampleRateAvg = params->sample_rate;

        } else {
            info.codecType = "other";
        }

        info.codecName = codec ? codec->name : "unknown";
        result.streams.push_back(std::move(info));
    }

    return result;
}

void print_info(FFProbeOutput const& info)
{
    std::cout << "Format:\n";
    std::cout << "  Duration: " << info.format.duration << " seconds\n";
    std::cout << "  Size: " << info.format.size << " bytes\n";
    std::cout << "  BitRate: " << info.format.bitRate << " b/s\n";

    for (auto const& s : info.streams) {
        std::cout << "Stream:\n";
        std::cout << "  CodecType: " << s.codecType << '\n';
        std::cout << "  CodecName: " << s.codecName << '\n';

        if (s.codecType == "video") {
            std::cout << "  Width: " << s.width << '\n';
            std::cout << "  Height: " << s.height << '\n';
            std::cout << "  AvgFrameRate: " << s.avgFrameRate.numerator << '/'
                      << s.avgFrameRate.denominator << '\n';
        }

        if (s.codecType == "audio") {
            std::cout << "  SampleRateAvg: " << s.sampleRateAvg << '\n';
        }
    }
}
