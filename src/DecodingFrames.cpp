#include <memory>

#include "DecodingFrames.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

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

void save_gray_frame(uint8_t const* buf, int wrap, int width, int height, std::string const& filename)
{
    std::FILE* f = std::fopen(filename.c_str(), "wb");
    if (!f)
        throw std::runtime_error("Failed to open file for writing: " + filename);

    // Write a minimal .pgm header
    std::fprintf(f, "P5\n%d %d\n255\n", width, height);

    // Write image data line by line
    for (int i = 0; i < height; i++) {
        std::fwrite(buf + i * wrap, 1, width, f);
    }
    std::fclose(f);
}

// Deleter functions for our smart pointers
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

} // namespace

void decode_and_save_video_frames(char const* input_file)
{
    using FormatContextPtr = std::unique_ptr<AVFormatContext, decltype(&free_format_ctx)>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, decltype(&free_codec_ctx)>;
    using PacketPtr = std::unique_ptr<AVPacket, decltype(&free_packet)>;
    using FramePtr = std::unique_ptr<AVFrame, decltype(&free_frame)>;

    logf("Opening input file: %s", input_file);

    AVFormatContext* rawCtx = nullptr;
    if (avformat_open_input(&rawCtx, input_file, nullptr, nullptr) < 0) {
        throw std::runtime_error("Failed to open input file");
    }

    FormatContextPtr formatCtx(rawCtx, free_format_ctx);

    if (avformat_find_stream_info(formatCtx.get(), nullptr) < 0) {
        throw std::runtime_error("Failed to find stream info");
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
    if (videoStreamIndex == -1)
        throw std::runtime_error("No video stream found in file");

    AVStream* videoStream = formatCtx->streams[videoStreamIndex];
    // Need time_base for timestamp to seconds conversion
    AVRational timeBase = videoStream->time_base;

    CodecContextPtr codecCtx(avcodec_alloc_context3(codec), free_codec_ctx);
    if (!codecCtx)
        throw std::runtime_error("Failed to allocate AVCodecContext");

    if (avcodec_parameters_to_context(codecCtx.get(), codecParams) < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }
    if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) {
        throw std::runtime_error("Failed to open codec");
    }

    FramePtr frame(av_frame_alloc(), free_frame);
    PacketPtr packet(av_packet_alloc(), free_packet);
    if (!frame || !packet) {
        throw std::runtime_error("Failed to allocate frame or packet");
    }

    double lastCaptureTime = -1.0;
    int framesCaptured = 0;

    constexpr int maxFramesToSave = 100;

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

                if ((framePtsSec - lastCaptureTime) >= 1.0 || lastCaptureTime < 0.0) {
                    lastCaptureTime = framePtsSec;

                    logf("Capturing frame at %.2f sec (frame_num=%d, format=%d)",
                        framePtsSec, codecCtx->frame_num, frame->format);

                    std::string filename = "frame-" + std::to_string(framesCaptured + 1) + ".pgm";
                    save_gray_frame(frame->data[0], frame->linesize[0], frame->width, frame->height, filename);
                    framesCaptured++;
                }
            }
        }
        // Unref the packet so we can reuse it
        av_packet_unref(packet.get());
    }

    logf("Captured %d keyframes at ~1s intervals", framesCaptured);
}

