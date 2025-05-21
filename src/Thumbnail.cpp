#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <spdlog/spdlog.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "Thumbnail.h"
#include "VideoInfo.h"

namespace {

inline void free_format_ctx(AVFormatContext* ctx)
{
    if (ctx)
        avformat_close_input(&ctx);
}

inline void free_codec_ctx(AVCodecContext* ctx)
{
    if (ctx)
        avcodec_free_context(&ctx);
}

inline void free_sws_ctx(SwsContext* ctx)
{
    if (ctx)
        sws_freeContext(ctx);
}

inline void fr_free(AVFrame* f)
{
    if (f)
        av_frame_free(&f);
}

inline void pk_free(AVPacket* p)
{
    if (p)
        av_packet_free(&p);
}

// Smart-pointer aliases that use the deleters above
using FormatCtxPtr = std::unique_ptr<AVFormatContext, decltype(&free_format_ctx)>;
using CtxPtr = std::unique_ptr<AVCodecContext, decltype(&free_codec_ctx)>;
using SwsContextPtr = std::unique_ptr<SwsContext, decltype(&free_sws_ctx)>;
using FrmPtr = std::unique_ptr<AVFrame, decltype(&fr_free)>;
using PktPtr = std::unique_ptr<AVPacket, decltype(&pk_free)>;

// Very small utility: SHA-1 of the path, returned as hex
inline QString hashPath(QString const& path)
{
    return QCryptographicHash::hash(path.toUtf8(),
        QCryptographicHash::Sha1)
        .toHex();
}

inline bool copyFrameToImage(AVFrame const* frame, QImage& image, int width, int height)
{
    if (!frame || !frame->data[0]) {
        spdlog::error("[Thumbnail] Invalid frame or null data pointer");
        return false;
    }

    try {
        // For 10-bit and other high bit-depth formats, we need to be careful with buffer sizes
        int const rowBytes = std::min<int>(frame->linesize[0],
            image.bytesPerLine());

        // Log frame details for diagnostics
        spdlog::debug("[Thumbnail] Frame format: linesize={}, width={}, height={}, format={}",
            frame->linesize[0], frame->width, frame->height, frame->format);

        // FFmpeg guarantees that all allocated lines are accessible
        // Using frame->height as the loop limit is safe
        for (int y = 0; y < height; ++y) {
            std::memcpy(image.scanLine(y),
                frame->data[0] + y * frame->linesize[0],
                rowBytes);
        }

        return true;
    } catch (std::exception const& ex) {
        spdlog::error("[Thumbnail] Exception during frame copy: {}", ex.what());
        return false;
    }
}

} // namespace

std::optional<std::vector<QString>>
extract_color_thumbnails(VideoInfo const& info,
    int thumbnailsToGenerate)
{
    spdlog::info("[Thumbnail] requested {} thumbnails", thumbnailsToGenerate);
    using namespace std::literals;
    std::string const& filePath = info.path;

    if (thumbnailsToGenerate <= 0) {
        spdlog::warn("[Thumbnail] thumbnailsToGenerate must be positive");
        return std::nullopt;
    }

    if (filePath.empty()) {
        spdlog::error("[Thumbnail] Empty file path provided");
        return std::nullopt;
    }

    std::vector<QString> results;
    results.reserve(thumbnailsToGenerate);

    // Prepare output directory and related variables once
    QString base = QFileInfo(QString::fromStdString(filePath)).baseName();
    QString hash = hashPath(QString::fromStdString(filePath));
    QDir outDir(QDir::current().filePath("thumbnails"));
    if (!outDir.exists() && !outDir.mkpath(".")) {
        spdlog::error("[Thumbnail] Failed to create directory: {}",
            outDir.absolutePath().toStdString());
        return std::nullopt;
    }

    try {
        // Open container
        AVFormatContext* rawFmt = nullptr;
        int openResult = avformat_open_input(&rawFmt, filePath.c_str(), nullptr, nullptr);
        if (openResult < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            av_strerror(openResult, errBuf, sizeof(errBuf));
            spdlog::warn("[Thumbnail] Failed to open: {} ({})", filePath, errBuf);
            return std::nullopt;
        }
        FormatCtxPtr fmtCtx(rawFmt, &free_format_ctx);

        int infoResult = avformat_find_stream_info(fmtCtx.get(), nullptr);
        if (infoResult < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            av_strerror(infoResult, errBuf, sizeof(errBuf));
            spdlog::warn("[Thumbnail] Could not read stream info: {} ({})", filePath, errBuf);
            return std::nullopt;
        }

        // Locate video stream
        int videoStreamIdx = -1;
        AVCodec const* codec = nullptr;
        AVCodecParameters* codecParams = nullptr;

        for (unsigned i = 0; i < fmtCtx->nb_streams && videoStreamIdx == -1; ++i) {
            AVStream* st = fmtCtx->streams[i];
            if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (AVCodec const* c = avcodec_find_decoder(st->codecpar->codec_id)) {
                    codec = c;
                    codecParams = st->codecpar;
                    videoStreamIdx = static_cast<int>(i);
                }
            }
        }

        if (videoStreamIdx < 0) {
            spdlog::warn("[Thumbnail] No video stream found in {}", filePath);
            return std::nullopt;
        }

        // Set up decoder
        CtxPtr codecCtx(avcodec_alloc_context3(codec), &free_codec_ctx);
        if (!codecCtx) {
            spdlog::error("[Thumbnail] Failed to allocate codec context for {}", filePath);
            return std::nullopt;
        }

        if (avcodec_parameters_to_context(codecCtx.get(), codecParams) < 0) {
            spdlog::warn("[Thumbnail] Failed to copy codec parameters for {}", filePath);
            return std::nullopt;
        }

        int openCodecResult = avcodec_open2(codecCtx.get(), codec, nullptr);
        if (openCodecResult < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            av_strerror(openCodecResult, errBuf, sizeof(errBuf));
            spdlog::warn("[Thumbnail] Failed to open codec: {} ({})", filePath, errBuf);
            return std::nullopt;
        }

        AVStream* vStream = fmtCtx->streams[videoStreamIdx];
        if (!vStream) {
            spdlog::error("[Thumbnail] Stream pointer is null after lookup");
            return std::nullopt;
        }

        //-------------------------------------------------------------
        // Compute total duration in seconds
        double durSec = info.duration;
        if (durSec <= 0.0 && fmtCtx->duration > 0)
            durSec = fmtCtx->duration / static_cast<double>(AV_TIME_BASE);

        // Pre-compute target PTS values (N thumbnails, skip first/last frame)
        std::vector<int64_t> targets;
        targets.reserve(thumbnailsToGenerate);
        for (int i = 0; i < thumbnailsToGenerate; ++i) {
            double secs = (i + 1) * durSec / (thumbnailsToGenerate + 1);
            int64_t pts = av_rescale_q(static_cast<int64_t>(secs * AV_TIME_BASE),
                AVRational { 1, AV_TIME_BASE },
                vStream->time_base);
            targets.push_back(pts);
        }
        constexpr int kMaxProbeFrames = 512; // big GOPs near the beginning need more packets
        //-------------------------------------------------------------

        // Add this before setting up the scaler
        spdlog::debug("[Thumbnail] Input format: codec={}, pix_fmt={}, width={}, height={}",
            codec->name, av_get_pix_fmt_name(codecCtx->pix_fmt),
            codecCtx->width, codecCtx->height);

        // Special handling for high bit-depth formats
        bool isHighBitDepth = false;
        switch (codecCtx->pix_fmt) {
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_YUV422P10LE:
        case AV_PIX_FMT_YUV444P10LE:
        case AV_PIX_FMT_YUV420P12LE:
        case AV_PIX_FMT_YUV422P12LE:
        case AV_PIX_FMT_YUV444P12LE:
            isHighBitDepth = true;
            spdlog::debug("[Thumbnail] Detected high bit-depth format: {}",
                av_get_pix_fmt_name(codecCtx->pix_fmt));
            break;
        default:
            break;
        }

        // Use a different scaling algorithm for high bit-depth content
        int scalingAlgorithm = isHighBitDepth ? SWS_BICUBIC : SWS_BILINEAR;

        // Set up scaler to RGB
        constexpr int kThumbW = 128;
        constexpr int kThumbH = 128;
        constexpr AVPixelFormat outPixFmt = AV_PIX_FMT_RGB24;

        SwsContextPtr swsCtx(
            sws_getContext(codecCtx->width, codecCtx->height,
                codecCtx->pix_fmt,
                kThumbW, kThumbH, outPixFmt,
                scalingAlgorithm, nullptr, nullptr, nullptr),
            &free_sws_ctx);

        if (!swsCtx) {
            spdlog::warn("[Thumbnail] sws_getContext failed for {}", filePath);
            return std::nullopt;
        }

        // Allocate frames and packet
        FrmPtr srcFrame(av_frame_alloc(), &fr_free);
        FrmPtr rgbFrame(av_frame_alloc(), &fr_free);
        PktPtr pkt(av_packet_alloc(), &pk_free);

        if (!srcFrame || !rgbFrame || !pkt) {
            spdlog::error("[Thumbnail] Failed to allocate frames or packet");
            return std::nullopt;
        }

        int const bufSize = av_image_get_buffer_size(outPixFmt, kThumbW, kThumbH, 1);
        if (bufSize <= 0) {
            spdlog::error("[Thumbnail] Invalid buffer size: {}", bufSize);
            return std::nullopt;
        }

        std::vector<uint8_t> rgbBuf(static_cast<size_t>(bufSize));
        int fillResult = av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize,
            rgbBuf.data(), outPixFmt,
            kThumbW, kThumbH, 1);

        if (fillResult < 0) {
            char errBuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            av_strerror(fillResult, errBuf, sizeof(errBuf));
            spdlog::error("[Thumbnail] Failed to fill image arrays: {}", errBuf);
            return std::nullopt;
        }

        rgbFrame->format = outPixFmt;
        rgbFrame->width = kThumbW;
        rgbFrame->height = kThumbH;

        // Loop over pre-computed targets
        for (int idx = 0; idx < thumbnailsToGenerate; ++idx) {

            avcodec_flush_buffers(codecCtx.get());

            if (avformat_seek_file(fmtCtx.get(), videoStreamIdx,
                    std::numeric_limits<int64_t>::min(),
                    targets[idx],
                    std::numeric_limits<int64_t>::max(),
                    AVSEEK_FLAG_BACKWARD)
                < 0) {
                spdlog::debug("[Thumbnail] seek failed at idx {}", idx);
                continue; // graceful skip
            }

            bool gotFrameForIdx = false;
            int probe = 0;
            while (probe < kMaxProbeFrames && av_read_frame(fmtCtx.get(), pkt.get()) >= 0) {
                ++probe;

                if (pkt->stream_index != videoStreamIdx) {
                    av_packet_unref(pkt.get());
                    continue;
                }

                if (pkt->size <= 0) {
                    spdlog::warn("[Thumbnail] Skipping invalid packet size: {}", pkt->size);
                    av_packet_unref(pkt.get());
                    continue;
                }

                int sendResult = avcodec_send_packet(codecCtx.get(), pkt.get());
                av_packet_unref(pkt.get()); // Always unref regardless of send result

                if (sendResult < 0) {
                    char errBuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
                    av_strerror(sendResult, errBuf, sizeof(errBuf));
                    spdlog::debug("[Thumbnail] Send packet error: {}", errBuf);
                    continue;
                }

                int receiveResult;
                while ((receiveResult = avcodec_receive_frame(codecCtx.get(), srcFrame.get())) >= 0) {
                    if (srcFrame->best_effort_timestamp == AV_NOPTS_VALUE || srcFrame->best_effort_timestamp < targets[idx]) {
                        continue; // still too early, keep decoding
                    }
                    // Scale the frame to thumbnail size
                    int scaleResult = sws_scale(swsCtx.get(),
                        srcFrame->data, srcFrame->linesize,
                        0, srcFrame->height,
                        rgbFrame->data, rgbFrame->linesize);

                    if (scaleResult <= 0) {
                        spdlog::warn("[Thumbnail] Failed to scale frame: {}", scaleResult);
                        continue;
                    }

                    // Convert to QImage
                    QImage img(kThumbW, kThumbH, QImage::Format_RGB888);

                    if (!copyFrameToImage(rgbFrame.get(), img, kThumbW, kThumbH)) {
                        spdlog::error("[Thumbnail] Failed to copy frame to QImage");
                        continue;
                    }

                    // Save thumbnail with a unique name based on file hash and index
                    QString thumbPath = outDir.filePath(
                        base + "_" + hash.left(8) + QString("_thumb-%1.jpg").arg(idx, 3, 10, QLatin1Char('0')));

                    int64_t frameDurationPts = 0;
                    if (vStream->avg_frame_rate.num > 0 && vStream->avg_frame_rate.den > 0)
                        frameDurationPts = av_rescale_q(1, av_inv_q(vStream->avg_frame_rate),
                            vStream->time_base);
                    else if (vStream->r_frame_rate.num > 0 && vStream->r_frame_rate.den > 0)
                        frameDurationPts = av_rescale_q(1, av_inv_q(vStream->r_frame_rate),
                            vStream->time_base);
                    // fallback: assume 30 fps
                    if (frameDurationPts == 0)
                        frameDurationPts = av_rescale_q(1, AVRational { 1, 30 }, vStream->time_base);

                    if (img.save(thumbPath, "JPEG", 85)) {
                        spdlog::debug("[Thumbnail] Successfully saved thumbnail: {}", thumbPath.toStdString());
                        results.push_back(thumbPath);
                        gotFrameForIdx = true;
                        break;
                    } else {
                        spdlog::error("[Thumbnail] Failed to save image at {}",
                            thumbPath.toStdString());
                        // don't return, just try next probe
                    }
                }

                if (gotFrameForIdx) {
                    break; // leave pkt-reading loop, move to next target idx
                }

                if (receiveResult < 0 && receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
                    char errBuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
                    av_strerror(receiveResult, errBuf, sizeof(errBuf));
                    spdlog::debug("[Thumbnail] Receive frame error: {}", errBuf);
                }
            }
        }

        if (!results.empty())
            return results;

        return std::nullopt;
    } catch (std::exception const& ex) {
        spdlog::error("[Thumbnail] Exception: {}", ex.what());
        return std::nullopt;
    } catch (...) {
        spdlog::error("[Thumbnail] Unknown exception while creating thumbnail");
        return std::nullopt;
    }
}
