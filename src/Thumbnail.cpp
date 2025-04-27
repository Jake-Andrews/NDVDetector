#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <cstring>
#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "Thumbnail.h"

namespace {

inline void free_format_ctx(AVFormatContext* ctx)
{
    if (ctx)
        avformat_close_input(&ctx); // takes **&ctx**
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

} // namespace

[[nodiscard]]
std::optional<QString> extract_color_thumbnail(std::string const& filePath)
{
    using namespace std::literals;

    // Open container
    AVFormatContext* rawFmt = nullptr;
    if (avformat_open_input(&rawFmt, filePath.c_str(), nullptr, nullptr) < 0) {
        qWarning() << "[Thumbnail] Failed to open:" << QString::fromStdString(filePath);
        return std::nullopt;
    }
    FormatCtxPtr fmtCtx(rawFmt, &free_format_ctx);

    if (avformat_find_stream_info(fmtCtx.get(), nullptr) < 0) {
        qWarning() << "[Thumbnail] Could not read stream info:"
                   << QString::fromStdString(filePath);
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
        qWarning() << "[Thumbnail] No video stream in" << QString::fromStdString(filePath);
        return std::nullopt;
    }

    // Set up decoder
    CtxPtr codecCtx(avcodec_alloc_context3(codec), &free_codec_ctx);
    if (!codecCtx || avcodec_parameters_to_context(codecCtx.get(), codecParams) < 0) {
        qWarning() << "[Thumbnail] Failed to create decoder for"
                   << QString::fromStdString(filePath);
        return std::nullopt;
    }

    if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) {
        qWarning() << "[Thumbnail] Failed to open codec for"
                   << QString::fromStdString(filePath);
        return std::nullopt;
    }

    // Seek to ~1 second
    AVStream* vStream = fmtCtx->streams[videoStreamIdx];
    AVRational tbase = vStream->time_base;
    int64_t const oneSecondPts = av_rescale_q(1, AVRational { 1, 1 }, tbase);

    auto try_seek = [&](int flags) {
        return avformat_seek_file(fmtCtx.get(), videoStreamIdx,
                   0, oneSecondPts, oneSecondPts, flags)
            >= 0;
    };

    if (!try_seek(AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_FRAME))
        if (!try_seek(AVSEEK_FLAG_ANY))
            av_seek_frame(fmtCtx.get(), videoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);

    avcodec_flush_buffers(codecCtx.get());

    // Set up scaler to RGB
    constexpr int kThumbW = 128;
    constexpr int kThumbH = 128;
    constexpr AVPixelFormat outPixFmt = AV_PIX_FMT_RGB24;

    SwsContextPtr swsCtx(
        sws_getContext(codecCtx->width, codecCtx->height,
            codecCtx->pix_fmt,
            kThumbW, kThumbH, outPixFmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr),
        &free_sws_ctx);
    if (!swsCtx) {
        qWarning() << "[Thumbnail] sws_getContext failed for"
                   << QString::fromStdString(filePath);
        return std::nullopt;
    }

    // Allocate frames and packet
    FrmPtr srcFrame(av_frame_alloc(), &fr_free);
    FrmPtr rgbFrame(av_frame_alloc(), &fr_free);
    PktPtr pkt(av_packet_alloc(), &pk_free);

    int const bufSize = av_image_get_buffer_size(outPixFmt, kThumbW, kThumbH, 1);
    std::vector<uint8_t> rgbBuf(static_cast<size_t>(bufSize));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize,
        rgbBuf.data(), outPixFmt,
        kThumbW, kThumbH, 1);

    // Decode and scale one frame
    while (av_read_frame(fmtCtx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != videoStreamIdx) {
            av_packet_unref(pkt.get());
            continue;
        }

        if (pkt->size <= 0) {
            spdlog::warn("[Thumbnail] Skipping invalid packet size: {}", pkt->size);
            av_packet_unref(pkt.get());
            continue;
        }

        if (avcodec_send_packet(codecCtx.get(), pkt.get()) < 0) {
            av_packet_unref(pkt.get());
            continue;
        }
        av_packet_unref(pkt.get());

        while (avcodec_receive_frame(codecCtx.get(), srcFrame.get()) >= 0) {
            sws_scale(swsCtx.get(),
                srcFrame->data, srcFrame->linesize,
                0, codecCtx->height,
                rgbFrame->data, rgbFrame->linesize);

            // Convert to QImage
            QImage img(kThumbW, kThumbH, QImage::Format_RGB888);
            int rowBytes = std::min(kThumbW * 3, rgbFrame->linesize[0]);
            for (int y = 0; y < kThumbH; ++y) {
                std::memcpy(img.scanLine(y),
                    rgbFrame->data[0] + y * rgbFrame->linesize[0],
                    rowBytes);
            }

            // Save thumbnail
            QString base = QFileInfo(QString::fromStdString(filePath)).baseName();
            QString hash = hashPath(QString::fromStdString(filePath));
            QDir outDir(QDir::current().filePath("thumbnails"));
            outDir.mkpath(".");

            QString thumbPath = outDir.filePath(base + "_" + hash.left(8) + "_thumb.jpg");

            if (img.save(thumbPath, "JPEG"))
                return thumbPath;

            qWarning() << "[Thumbnail] Failed to save image at" << thumbPath;
            return std::nullopt;
        }
    }

    qWarning() << "[Thumbnail] No decodable frame found in"
               << QString::fromStdString(filePath);
    return std::nullopt;
}
