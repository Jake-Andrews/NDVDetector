#include "DecodingFrames.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <iostream>
#include <memory>
#include <qdebug.h>
#include <qglobal.h>
#include <qimage.h>
#include <qobject.h>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
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
void free_sws_ctx(SwsContext* ctx)
{
    if (ctx)
        sws_freeContext(ctx);
}

QString hashPath(QString const& path)
{
    return QString(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex());
}

}

// **Fix: Support for channels**
std::vector<CImg<float>> decode_video_frames_as_cimg(std::string const& file_path)
{
    using FormatContextPtr = std::unique_ptr<AVFormatContext, decltype(&free_format_ctx)>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, decltype(&free_codec_ctx)>;
    using PacketPtr = std::unique_ptr<AVPacket, decltype(&free_packet)>;
    using FramePtr = std::unique_ptr<AVFrame, decltype(&free_frame)>;
    using SwsContextPtr = std::unique_ptr<SwsContext, decltype(&free_sws_ctx)>;

    std::vector<CImg<float>> frames;

    logf("Opening input file: %s", file_path.c_str());

    // 1) Open file
    AVFormatContext* rawCtx = nullptr;
    if (avformat_open_input(&rawCtx, file_path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[Error] Failed to open input file: " << file_path << "\n";
        return {};
    }

    FormatContextPtr formatCtx(rawCtx, free_format_ctx);

    // 2) Gather stream info
    if (avformat_find_stream_info(formatCtx.get(), nullptr) < 0) {
        std::cerr << "[Error] Failed to find stream info: " << file_path << "\n";
        return {};
    }

    AVCodec const* codec = nullptr;
    AVCodecParameters* codecParams = nullptr;
    int videoStreamIndex = -1;

    // 3) Find video stream
    for (unsigned int i = 0; i < formatCtx->nb_streams; ++i) {
        auto* stream = formatCtx->streams[i];
        if (!stream || !stream->codecpar)
            continue;
        auto* localCodec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!localCodec)
            continue;

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = static_cast<int>(i);
            codec = localCodec;
            codecParams = stream->codecpar;
            break;
        }
    }
    if (videoStreamIndex == -1) {
        std::cerr << "[Error] No video stream found in file: " << file_path << "\n";
        return {};
    }

    // 4) Allocate and open codec
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

    SwsContextPtr swsCtx(sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt, 32, 32, AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr), free_sws_ctx);
    if (!swsCtx) {
        std::cerr << "[Error] Failed to create SwsContext.\n";
    }

    // 5) Allocate frame & packet
    FramePtr frame(av_frame_alloc(), free_frame);
    if (!frame) {
        std::cerr << "[Error] Failed to allocate frame.\n";
        return {};
    }
    frame->format = AV_PIX_FMT_GRAY8;
    frame->width = 32;
    frame->height = 32;
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        std::cerr << "[Error] Failed to allocate buffer for scaled frame.\n";
        return {};
    }

    PacketPtr packet(av_packet_alloc(), free_packet);
    if (!packet) {
        std::cerr << "[Error] Failed to allocate packet.\n";
        return {};
    }

    double lastCaptureTime = -1.0;
    int framesCaptured = 0;
    constexpr int maxFramesToSave = 100;
    auto const timeBase = formatCtx->streams[videoStreamIndex]->time_base;

    // 6) Read and decode frames
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
                    break; // Need more packets or end of file
                }
                if (ret < 0) {
                    logf("Error receiving frame: %s", av_err_to_string(ret).c_str());
                    break;
                }

                double framePtsSec = frame->pts * av_q2d(timeBase);

                // Capture 1 frame every ~1 second
                if ((framePtsSec - lastCaptureTime) >= 1.0 || lastCaptureTime < 0.0) {
                    lastCaptureTime = framePtsSec;

                    logf("Capturing frame at %.2f sec (frame_num=%d, format=%d)",
                        framePtsSec, codecCtx->frame_num, frame->format);

                    sws_scale(
                        swsCtx.get(),
                        frame->data,
                        frame->linesize,
                        0,
                        frame->height,
                        frame->data,
                        frame->linesize);

                    // Now build a CImg from frame
                    CImg<float> img(32, 32, 1, 1, 0.0f);
                    uint8_t const* basePtr = frame->data[0];
                    int lineSize = frame->linesize[0];

                    for (int row = 0; row < 32; ++row) {
                        auto const* src = basePtr + row * lineSize;
                        for (int col = 0; col < 32; ++col) {
                            img(col, row) = static_cast<float>(src[col]);
                        }
                    }
                    frames.push_back(std::move(img));
                    framesCaptured++;
                }
            }
        }
        av_packet_unref(packet.get());
    }

    logf("Captured %d frames at ~1s intervals", framesCaptured);
    return frames;
}

std::optional<QString> extract_color_thumbnail(std::string const& filePath)
{
    using FormatContextPtr = std::unique_ptr<AVFormatContext, decltype(&free_format_ctx)>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, decltype(&free_codec_ctx)>;
    using FramePtr = std::unique_ptr<AVFrame, decltype(&free_frame)>;
    using PacketPtr = std::unique_ptr<AVPacket, decltype(&free_packet)>;
    using SwsContextPtr = std::unique_ptr<SwsContext, decltype(&free_sws_ctx)>;

    // 1) Open input
    AVFormatContext* rawFmt = nullptr;
    if (avformat_open_input(&rawFmt, filePath.c_str(), nullptr, nullptr) < 0) {
        qWarning() << "[Error] Could not open input for thumbnail:" << QString::fromStdString(filePath);
        return std::nullopt;
    }
    FormatContextPtr fmtCtx(rawFmt, free_format_ctx);

    if (avformat_find_stream_info(fmtCtx.get(), nullptr) < 0) {
        qWarning() << "[Error] Could not find stream info for thumbnail:" << QString::fromStdString(filePath);
        return std::nullopt;
    }

    // 2) Find video stream
    int videoStreamIndex = -1;
    AVCodec const* codec = nullptr;
    AVCodecParameters* codecParams = nullptr;

    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        auto* st = fmtCtx->streams[i];
        if (!st || !st->codecpar)
            continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            codec = avcodec_find_decoder(st->codecpar->codec_id);
            if (codec) {
                codecParams = st->codecpar;
                videoStreamIndex = static_cast<int>(i);
                break;
            }
        }
    }

    if (videoStreamIndex == -1) {
        qWarning() << "[Error] No video stream found in" << QString::fromStdString(filePath);
        return std::nullopt;
    }

    // 3) Create and open codec context
    CodecContextPtr codecCtx(avcodec_alloc_context3(codec), free_codec_ctx);
    if (avcodec_parameters_to_context(codecCtx.get(), codecParams) < 0) {
        qWarning() << "[Error] Failed to copy codec parameters for" << QString::fromStdString(filePath);
        return std::nullopt;
    }
    if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) {
        qWarning() << "[Error] Could not open codec for" << QString::fromStdString(filePath);
        return std::nullopt;
    }

    // 4) Create swscale context
    SwsContextPtr swsCtx(
        sws_getContext(codecCtx->width,
            codecCtx->height,
            codecCtx->pix_fmt,
            128,
            128,
            AV_PIX_FMT_RGB24,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr),
        free_sws_ctx);

    if (!swsCtx) {
        qWarning() << "[Error] Could not create SWS context for thumbnail";
        return std::nullopt;
    }

    FramePtr frame(av_frame_alloc(), free_frame);
    FramePtr frameRGB(av_frame_alloc(), free_frame);
    PacketPtr packet(av_packet_alloc(), free_packet);

    // Allocate buffers for frameRGB
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, 128, 128, 1);
    std::vector<uint8_t> buffer(numBytes);

    av_image_fill_arrays(frameRGB->data, frameRGB->linesize,
        buffer.data(),
        AV_PIX_FMT_RGB24,
        128,
        128,
        1);

    // 5) Read and process frames
    bool gotThumbnail = false;
    while (av_read_frame(fmtCtx.get(), packet.get()) >= 0 && !gotThumbnail) {
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecCtx.get(), packet.get()) < 0)
                break;

            while (true) {
                int ret = avcodec_receive_frame(codecCtx.get(), frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                else if (ret < 0) {
                    qWarning() << "[Error] Error receiving thumbnail frame";
                    break;
                }

                // Scale to RGB24 128x128
                sws_scale(swsCtx.get(),
                    frame->data,
                    frame->linesize,
                    0,
                    codecCtx->height,
                    frameRGB->data,
                    frameRGB->linesize);

                // Convert to QImage
                QImage image(128, 128, QImage::Format_RGB888);
                for (int y = 0; y < 128; ++y) {
                    uint8_t* srcLine = frameRGB->data[0] + y * frameRGB->linesize[0];
                    memcpy(image.scanLine(y), srcLine, 128 * 3);
                }

                // Save thumbnail
                QString baseName = QFileInfo(QString::fromStdString(filePath)).baseName();
                QString hash = hashPath(QString::fromStdString(filePath));
                QString outDir = QDir::currentPath() + "/thumbnails";
                QDir().mkpath(outDir);
                QString thumbPath = outDir + "/" + baseName + "_" + hash.left(8) + "_thumb.jpg";

                if (image.save(thumbPath, "JPEG")) {
                    gotThumbnail = true;
                    av_packet_unref(packet.get());
                    return thumbPath;
                }
            }
        }
        av_packet_unref(packet.get());
    }

    return std::nullopt;
}
