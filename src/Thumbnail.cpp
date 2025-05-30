#include "Thumbnail.h"
#include "VideoInfo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <spdlog/spdlog.h>
#include <vector>

#include "VideoProcessingUtils.h"
using namespace vpu;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

struct SwsDeleter {
    void operator()(SwsContext* c) const noexcept
    {
        if (c)
            sws_freeContext(c);
    }
};
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

// SHA-1 of the path, returned as hex
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
        FmtPtr fmtCtx(rawFmt);

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
        CtxPtr codecCtx(avcodec_alloc_context3(codec));
        if (!codecCtx) {
            spdlog::error("[Thumbnail] Failed to allocate codec context for {}", filePath);
            return std::nullopt;
        }

        if (avcodec_parameters_to_context(codecCtx.get(), codecParams) < 0) {
            spdlog::warn("[Thumbnail] Failed to copy codec parameters for {}", filePath);
            return std::nullopt;
        }

        // optimisation flags – match FastVideoProcessor
        codecCtx->thread_type = FF_THREAD_FRAME;
        codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
        codecCtx->skip_idct = AVDISCARD_ALL;
        codecCtx->skip_loop_filter = AVDISCARD_ALL;
        unsigned tc = std::thread::hardware_concurrency();
        codecCtx->thread_count = tc ? tc : 1; // ≥1 thread

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

        // --- obtain clip duration (seconds) --------------------------
        double durSec = 0.0;
        if (fmtCtx->duration != AV_NOPTS_VALUE && fmtCtx->duration > 0)
            durSec = fmtCtx->duration / static_cast<double>(AV_TIME_BASE);
        else if (vStream && vStream->duration > 0)
            durSec = vStream->duration * av_q2d(vStream->time_base);
        if (durSec <= 0.0) { // fallback – avoid div/0
            spdlog::warn("[Thumbnail] Unknown duration for {}, using 1s", filePath);
            durSec = 1.0;
        }

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

        SwsPtr swsCtx(
            sws_getContext(codecCtx->width, codecCtx->height,
                codecCtx->pix_fmt,
                kThumbW, kThumbH, outPixFmt,
                scalingAlgorithm, nullptr, nullptr, nullptr));

        if (!swsCtx) {
            spdlog::warn("[Thumbnail] sws_getContext failed for {}", filePath);
            return std::nullopt;
        }

        // Allocate frames and packet
        FrmPtr srcFrame(av_frame_alloc());
        FrmPtr rgbFrame(av_frame_alloc());
        PktPtr pkt(av_packet_alloc());

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

            bool gotFrameForIdx = false;

            // seek to closest key-frame around target pts
            if (av_seek_frame(fmtCtx.get(),
                    videoStreamIdx,
                    targets[idx],
                    AVSEEK_FLAG_ANY)
                < 0) {
                spdlog::warn("[Thumbnail] seek failed for thumb {}", idx);
                continue;
            }
            avcodec_flush_buffers(codecCtx.get());
            avformat_flush(fmtCtx.get());

            // read packets until first decoded frame of this stream
            while (!gotFrameForIdx && av_read_frame(fmtCtx.get(), pkt.get()) >= 0) {
                if (pkt->stream_index != videoStreamIdx) {
                    av_packet_unref(pkt.get());
                    continue;
                }
                if (avcodec_send_packet(codecCtx.get(), pkt.get()) < 0) {
                    av_packet_unref(pkt.get());
                    continue;
                }
                av_packet_unref(pkt.get());

                while (avcodec_receive_frame(codecCtx.get(), srcFrame.get()) >= 0) {
                    // scale
                    if (sws_scale(swsCtx.get(),
                            srcFrame->data, srcFrame->linesize,
                            0, srcFrame->height,
                            rgbFrame->data, rgbFrame->linesize)
                        <= 0) {
                        av_frame_unref(srcFrame.get());
                        continue;
                    }

                    QImage img(kThumbW, kThumbH, QImage::Format_RGB888);
                    if (!copyFrameToImage(rgbFrame.get(), img, kThumbW, kThumbH)) {
                        av_frame_unref(srcFrame.get());
                        continue;
                    }

                    QString thumbPath = outDir.filePath(
                        base + "_" + hash.left(8) + QString("_thumb-%1.jpg").arg(idx, 3, 10, QLatin1Char('0')));
                    if (img.save(thumbPath, "JPEG", 85)) {
                        results.push_back(thumbPath);
                        gotFrameForIdx = true;
                    }
                    av_frame_unref(srcFrame.get());
                    break; // first frame only
                }
            }

            if (!gotFrameForIdx)
                spdlog::warn("[Thumbnail] Could not create thumbnail {} for '{}'", idx, filePath);
        }

        return results.empty() ? std::nullopt
                               : std::optional<std::vector<QString>>(std::move(results));
    } catch (std::exception const& ex) {
        spdlog::error("[Thumbnail] Exception: {}", ex.what());
        return std::nullopt;
    } catch (...) {
        spdlog::error("[Thumbnail] Unknown exception while creating thumbnail");
        return std::nullopt;
    }
}

std::optional<std::vector<QString>>
extract_color_thumbnails_precise(VideoInfo const& info,
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
        FmtPtr fmtCtx(rawFmt);

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
        CtxPtr codecCtx(avcodec_alloc_context3(codec));
        if (!codecCtx) {
            spdlog::error("[Thumbnail] Failed to allocate codec context for {}", filePath);
            return std::nullopt;
        }

        if (avcodec_parameters_to_context(codecCtx.get(), codecParams) < 0) {
            spdlog::warn("[Thumbnail] Failed to copy codec parameters for {}", filePath);
            return std::nullopt;
        }

        codecCtx->thread_type = FF_THREAD_FRAME;
        // codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
        codecCtx->skip_idct = AVDISCARD_ALL;
        // codecCtx->skip_loop_filter = AVDISCARD_ALL;
        unsigned tc = std::thread::hardware_concurrency();
        codecCtx->thread_count = tc ? tc : 1; // ≥1 thread

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

        // --- obtain clip duration (seconds) --------------------------
        double durSec = 0.0;
        if (fmtCtx->duration != AV_NOPTS_VALUE && fmtCtx->duration > 0)
            durSec = fmtCtx->duration / static_cast<double>(AV_TIME_BASE);
        else if (vStream && vStream->duration > 0)
            durSec = vStream->duration * av_q2d(vStream->time_base);
        if (durSec <= 0.0) { // fallback – avoid div/0
            spdlog::warn("[Thumbnail] Unknown duration for {}, using 1s", filePath);
            durSec = 1.0;
        }

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

        SwsPtr swsCtx(
            sws_getContext(codecCtx->width, codecCtx->height,
                codecCtx->pix_fmt,
                kThumbW, kThumbH, outPixFmt,
                scalingAlgorithm, nullptr, nullptr, nullptr));

        if (!swsCtx) {
            spdlog::warn("[Thumbnail] sws_getContext failed for {}", filePath);
            return std::nullopt;
        }

        // Allocate frames and packet
        FrmPtr srcFrame(av_frame_alloc());
        FrmPtr rgbFrame(av_frame_alloc());
        PktPtr pkt(av_packet_alloc());

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

            bool gotFrameForIdx = false;

            // seek to closest key-frame around target pts
            if (av_seek_frame(fmtCtx.get(),
                    videoStreamIdx,
                    targets[idx],
                    AVSEEK_FLAG_ANY)
                < 0) {
                spdlog::warn("[Thumbnail] seek failed for thumb {}", idx);
                continue;
            }
            avcodec_flush_buffers(codecCtx.get());
            avformat_flush(fmtCtx.get());

            // read packets until first decoded frame of this stream
            while (!gotFrameForIdx && av_read_frame(fmtCtx.get(), pkt.get()) >= 0) {
                if (pkt->stream_index != videoStreamIdx) {
                    av_packet_unref(pkt.get());
                    continue;
                }
                if (avcodec_send_packet(codecCtx.get(), pkt.get()) < 0) {
                    av_packet_unref(pkt.get());
                    continue;
                }
                av_packet_unref(pkt.get());

                while (avcodec_receive_frame(codecCtx.get(), srcFrame.get()) >= 0) {
                    // scale
                    if (sws_scale(swsCtx.get(),
                            srcFrame->data, srcFrame->linesize,
                            0, srcFrame->height,
                            rgbFrame->data, rgbFrame->linesize)
                        <= 0) {
                        av_frame_unref(srcFrame.get());
                        continue;
                    }

                    QImage img(kThumbW, kThumbH, QImage::Format_RGB888);
                    if (!copyFrameToImage(rgbFrame.get(), img, kThumbW, kThumbH)) {
                        av_frame_unref(srcFrame.get());
                        continue;
                    }

                    QString thumbPath = outDir.filePath(
                        base + "_" + hash.left(8) + QString("_thumb-%1.jpg").arg(idx, 3, 10, QLatin1Char('0')));
                    if (img.save(thumbPath, "JPEG", 85)) {
                        results.push_back(thumbPath);
                        gotFrameForIdx = true;
                    }
                    av_frame_unref(srcFrame.get());
                    break; // first frame only
                }
            }

            if (!gotFrameForIdx)
                spdlog::warn("[Thumbnail] Could not create thumbnail {} for '{}'", idx, filePath);
        }

        return results.empty() ? std::nullopt
                               : std::optional<std::vector<QString>>(std::move(results));
    } catch (std::exception const& ex) {
        spdlog::error("[Thumbnail] Exception: {}", ex.what());
        return std::nullopt;
    } catch (...) {
        spdlog::error("[Thumbnail] Unknown exception while creating thumbnail");
        return std::nullopt;
    }
}
