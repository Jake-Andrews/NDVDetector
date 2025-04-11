#include "DecodingFrames.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace {

void logf(char const* fmt, ...)
{
    va_list args;
    std::fprintf(stderr, "LOG: ");
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

std::string av_err_to_string(int errnum)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buffer, sizeof(buffer));
    return std::string(buffer);
}

bool save_gray_frame(uint8_t const* buf, int wrap, int width, int height, std::string const& filename)
{
    if (auto* f = std::fopen(filename.c_str(), "wb")) {
        std::fprintf(f, "P5\n%d %d\n255\n", width, height);

        for (int i = 0; i < height; i++) {
            std::fwrite(buf + i * wrap, 1, width, f);
        }
        std::fclose(f);
        return true;
    }
    std::cerr << "[Error] Failed to open file for writing: " << filename << "\n";
    return false;
}

void free_format_ctx(AVFormatContext* ctx)
{
    if (ctx)
        avformat_close_input(&ctx);
}
void free_codec_ctx(AVCodecContext* ctx)
{
    if (ctx)
        avcodec_free_context(&ctx);
}
void free_packet(AVPacket* pkt)
{
    if (pkt)
        av_packet_free(&pkt);
}
void free_frame(AVFrame* frame)
{
    if (frame)
        av_frame_free(&frame);
}

}

std::vector<std::string> decode_and_save_video_frames(std::filesystem::path const& input_file)
{
    using FormatContextPtr = std::unique_ptr<AVFormatContext, decltype(&free_format_ctx)>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, decltype(&free_codec_ctx)>;
    using PacketPtr = std::unique_ptr<AVPacket, decltype(&free_packet)>;
    using FramePtr = std::unique_ptr<AVFrame, decltype(&free_frame)>;

    std::vector<std::string> screenshotPaths;

    logf("Opening input file: %s", input_file.string().c_str());

    AVFormatContext* rawCtx = nullptr;
    if (avformat_open_input(&rawCtx, input_file.string().c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[Error] Failed to open input file: " << input_file << "\n";
        return {};
    }

    FormatContextPtr formatCtx(rawCtx, free_format_ctx);

    if (avformat_find_stream_info(formatCtx.get(), nullptr) < 0) {
        std::cerr << "[Error] Failed to find stream info: " << input_file << "\n";
        return {};
    }

    AVCodec const* codec = nullptr;
    AVCodecParameters* codecParams = nullptr;
    int videoStreamIndex = -1;

    for (unsigned int i = 0; i < formatCtx->nb_streams; ++i) {
        auto* stream = formatCtx->streams[i];
        auto* localParams = stream->codecpar;
        auto* localCodec = avcodec_find_decoder(localParams->codec_id);

        if (!localCodec)
            continue;
        if (localParams->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            codec = localCodec;
            codecParams = localParams;
            break;
        }
    }
    if (videoStreamIndex == -1) {
        std::cerr << "[Error] No video stream found in file: " << input_file << "\n";
        return {};
    }

    AVStream* videoStream = formatCtx->streams[videoStreamIndex];
    AVRational timeBase = videoStream->time_base;

    CodecContextPtr codecCtx(avcodec_alloc_context3(codec), free_codec_ctx);
    if (!codecCtx) {
        std::cerr << "[Error] Failed to allocate AVCodecContext.\n";
        return {};
    }

    if (avcodec_parameters_to_context(codecCtx.get(), codecParams) < 0) {
        std::cerr << "[Error] Failed to copy codec parameters.\n";
        return {};
    }
    if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) {
        std::cerr << "[Error] Failed to open codec.\n";
        return {};
    }

    FramePtr frame(av_frame_alloc(), free_frame);
    PacketPtr packet(av_packet_alloc(), free_packet);
    if (!frame || !packet) {
        std::cerr << "[Error] Failed to allocate frame or packet.\n";
        return {};
    }

    double lastCaptureTime = -1.0;
    int framesCaptured = 0;
    constexpr int maxFramesToSave = 100;

    // frame by frame
    while (av_read_frame(formatCtx.get(), packet.get()) >= 0 && framesCaptured < maxFramesToSave) {
        if (packet->stream_index == videoStreamIndex) {

            int ret = avcodec_send_packet(codecCtx.get(), packet.get());
            if (ret < 0) {
                logf("Error sending packet: %s", av_err_to_string(ret).c_str());
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codecCtx.get(), frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    // Need more packets or we're at the end
                    break;
                }
                if (ret < 0) {
                    logf("Error receiving frame: %s", av_err_to_string(ret).c_str());
                    break;
                }

                double framePtsSec = frame->pts * av_q2d(timeBase);

                // Save 1 frame every ~1 second
                if ((framePtsSec - lastCaptureTime) >= 1.0 || lastCaptureTime < 0.0) {
                    lastCaptureTime = framePtsSec;

                    logf("Capturing frame at %.2f sec (frame_num=%d, format=%d)",
                        framePtsSec, codecCtx->frame_num, frame->format);

                    // name the file e.g. "frame-1.pgm"
                    std::string filename = "frame-" + std::to_string(framesCaptured + 1) + ".pgm";
                    if (save_gray_frame(frame->data[0], frame->linesize[0],
                            frame->width, frame->height, filename)) {
                        screenshotPaths.push_back(filename);
                        framesCaptured++;
                    }
                }
            }
        }
        av_packet_unref(packet.get());
    }

    logf("Captured %d keyframes at ~1s intervals", framesCaptured);
    return screenshotPaths;
}
