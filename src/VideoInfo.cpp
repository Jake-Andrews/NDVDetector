#include "VideoInfo.h"
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
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

void remove_already_processed(std::vector<VideoInfo>& fs_videos, std::vector<VideoInfo> const& db_videos)
{
    // Store already processed paths in a set for fast lookup
    std::unordered_set<std::filesystem::path> knownPaths;
    knownPaths.reserve(db_videos.size());

    for (auto const& v : db_videos) {
        knownPaths.insert(v.path);
    }

    // Remove any fs_videos whose path already exists in knownPaths
    auto new_end = std::remove_if(fs_videos.begin(), fs_videos.end(),
        [&](VideoInfo const& v) {
            std::filesystem::path p = v.path;
            if (knownPaths.contains(p)) {
                std::cout << "[Info] Skipping already processed: " << p << '\n';
                return true;
            }
            return false;
        });

    fs_videos.erase(new_end, fs_videos.end());
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
