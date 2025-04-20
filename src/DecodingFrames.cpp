#include "DecodingFrames.h"
#include "GPUVendor.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

QString hashPath(QString const& path)
{
    return QString(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex());
}

namespace {

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

void free_frame(AVFrame* frame)
{
    if (frame)
        av_frame_free(&frame);
}

void free_packet(AVPacket* pkt)
{
    if (pkt)
        av_packet_free(&pkt);
}

void free_sws_ctx(SwsContext* sws)
{
    if (sws)
        sws_freeContext(sws);
}

void free_bufref(AVBufferRef* ref)
{
    if (ref)
        av_buffer_unref(&ref);
}

using FormatCtxPtr = std::unique_ptr<AVFormatContext, decltype(&free_format_ctx)>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext, decltype(&free_codec_ctx)>;
using FramePtr = std::unique_ptr<AVFrame, decltype(&free_frame)>;
using PacketPtr = std::unique_ptr<AVPacket, decltype(&free_packet)>;
using SwsContextPtr = std::unique_ptr<SwsContext, decltype(&free_sws_ctx)>;
using BufRefPtr = std::unique_ptr<AVBufferRef, decltype(&free_bufref)>;

inline char const* av_err2str_wrap(int err)
{
    static thread_local char str[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, err);
}

constexpr int kProbeSize = 10 * 1024 * 1024;   // 10 MB
constexpr int kAnalyzeDuration = 10 * 1000000; // 10 s (µs)

} // namespace

std::vector<cimg_library::CImg<float>>
decode_video_frames_as_cimg(std::string const& file_path,
    double skip_percent,
    int video_duration_sec,
    AVHWDeviceType hwBackend,
    int max_frames)
{
    using namespace cimg_library;
    constexpr int kMaxConsecutiveErrors = 8;
    constexpr int kMaxTotalErrors = 32;
    constexpr auto kOutPix = AV_PIX_FMT_GRAY8;
    constexpr int kOutW = 32, kOutH = 32;

    enum class DecodeBackend { Unknown,
        Hardware,
        Software };
    DecodeBackend backendUsed = DecodeBackend::Unknown;

    // Output container
    std::vector<CImg<float>> frames;
    frames.reserve(max_frames > 0 ? max_frames : 64);

    // Time window to sample
    skip_percent = std::clamp(skip_percent, 0.0, 0.40);
    double start_s = video_duration_sec > 0 ? video_duration_sec * skip_percent : 0.0;
    double end_s = video_duration_sec > 0 ? video_duration_sec * (1.0 - skip_percent) : std::numeric_limits<double>::max();

    // Open container
    AVDictionary* fmt_opts = nullptr;
    av_dict_set_int(&fmt_opts, "probesize", kProbeSize, 0);
    av_dict_set_int(&fmt_opts, "analyzeduration", kAnalyzeDuration, 0);

    int ret = 0;
    AVFormatContext* raw_fmt = nullptr;
    if ((ret = avformat_open_input(&raw_fmt, file_path.c_str(), nullptr, &fmt_opts)) < 0) {
        spdlog::error("[decode] Cannot open '{}': {}", file_path, av_err2str_wrap(ret));
        return frames;
    }
    FormatCtxPtr fmt(raw_fmt, &free_format_ctx);
    av_dict_free(&fmt_opts);

    fmt->flags |= AVFMT_FLAG_GENPTS;

    if ((ret = avformat_find_stream_info(fmt.get(), nullptr)) < 0) {
        spdlog::error("[decode] Could not read stream info '{}': {}", file_path, av_err2str_wrap(ret));
        return frames;
    }

    // Pick stream / decoder
    int const video_idx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx < 0) {
        spdlog::warn("[decode] No video stream in '{}'", file_path);
        return frames;
    }

    AVStream* st = fmt->streams[video_idx];
    AVCodecID cid = st->codecpar->codec_id;
    AVCodec const* dec = avcodec_find_decoder(cid);
    if (!dec) {
        spdlog::warn("[decode] No decoder for '{}' in '{}'", avcodec_get_name(cid), file_path);
        return frames;
    }

    // Codec‑context factory
    auto makeCodecCtx = [&](bool want_hw) -> CodecCtxPtr {
        CodecCtxPtr c(avcodec_alloc_context3(dec), &free_codec_ctx);
        if (!c)
            return CodecCtxPtr(nullptr, &free_codec_ctx);
        if ((ret = avcodec_parameters_to_context(c.get(), st->codecpar)) < 0) {
            spdlog::error("[decode] avcodec_parameters_to_context: {}", av_err2str_wrap(ret));
            return CodecCtxPtr(nullptr, &free_codec_ctx);
        }

        c->err_recognition = AV_EF_CAREFUL | AV_EF_CRCCHECK;

        // if software, use all cores; if hardware, leave 0 (=driver default)
        c->thread_type = FF_THREAD_FRAME;
        c->thread_count = want_hw ? 0 : std::max(1u, std::thread::hardware_concurrency());

        return c;
    };

    CodecCtxPtr codecCtx(nullptr, &free_codec_ctx);
    BufRefPtr hwDev(nullptr, &free_bufref);
    AVPixelFormat hwFmt = AV_PIX_FMT_NONE;

    auto openCodec = [&](CodecCtxPtr& ctx, AVDictionary* d) -> bool {
        int r = avcodec_open2(ctx.get(), dec, &d);
        av_dict_free(&d);
        if (r < 0) {
            spdlog::warn("[decode] avcodec_open2 failed: {}", av_err2str_wrap(r));
            return false;
        }
        return true;
    };

    // Attempt hardware decoding setup
    bool hwOpened = false;
    if (hwBackend != AV_HWDEVICE_TYPE_NONE) {
        for (int i = 0;; ++i) {
            AVCodecHWConfig const* cfg = avcodec_get_hw_config(dec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == hwBackend) {
                hwFmt = cfg->pix_fmt;
                break;
            }
        }

        if (hwFmt != AV_PIX_FMT_NONE) {
            AVBufferRef* tmp = nullptr;
            if (av_hwdevice_ctx_create(&tmp, hwBackend, nullptr, nullptr, 0) == 0) {
                hwDev.reset(tmp);

                codecCtx = makeCodecCtx(true);
                if (codecCtx) {
                    codecCtx->hw_device_ctx = av_buffer_ref(hwDev.get());
                    codecCtx->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(hwFmt));
                    codecCtx->get_format = [](AVCodecContext* c, AVPixelFormat const* fmts) {
                        AVPixelFormat want = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(c->opaque));
                        for (auto p = fmts; *p != AV_PIX_FMT_NONE; ++p)
                            if (*p == want)
                                return *p;
                        return fmts[0];
                    };
                    if (openCodec(codecCtx, nullptr)) {
                        hwOpened = true;
                        backendUsed = DecodeBackend::Hardware;
                    }
                }
            }
        }
    }

    // Fallback to software decoding if hardware decoding failed
    if (!hwOpened) {
        spdlog::info("[decode] Falling back to software for '{}'", file_path);
        codecCtx = makeCodecCtx(false);
        if (!codecCtx || !openCodec(codecCtx, nullptr)) {
            spdlog::warn("[decode] Software fallback also failed for '{}'", file_path);
            return frames;
        }
        backendUsed = DecodeBackend::Software;
    }

    // backend logger
    bool backendLogged = false;
    auto log_backend = [&] {
        if (backendLogged)
            return;
        backendLogged = true;
        if (backendUsed == DecodeBackend::Hardware)
            spdlog::info("[decode] Final backend: Hardware ({}) for '{}'",
                av_hwdevice_get_type_name(hwBackend), file_path);
        else if (backendUsed == DecodeBackend::Software)
            spdlog::info("[decode] Final backend: Software ({}) for '{}'",
                avcodec_get_name(cid), file_path);
        else
            spdlog::warn("[decode] Final backend: Unknown for '{}'", file_path);
    };

    // Helpers to decide if a frame is still hardware‑accelerated
    auto really_used_hw = [&](AVFrame const* f) {
        return backendUsed == DecodeBackend::Hardware && f->format == hwFmt;
    };

    // Seek / time calculations
    AVRational tb = st->time_base;
    auto sec2pts = [&](double s) { return llround(s / av_q2d(tb)); };
    int64_t startPts = sec2pts(start_s);
    int64_t endPts = sec2pts(end_s);
    int64_t stepPts = sec2pts(1.0); // sample @ 1 fps
    int64_t nextCapture = startPts;

    if (startPts > 0 && av_seek_frame(fmt.get(), video_idx, startPts, AVSEEK_FLAG_BACKWARD) >= 0)
        avcodec_flush_buffers(codecCtx.get());

    // Scaler managed by unique_ptr
    using SwsPtr = std::unique_ptr<SwsContext, decltype(&sws_freeContext)>;
    SwsPtr sws(nullptr, &sws_freeContext);
    int srcW = 0, srcH = 0;
    AVPixelFormat srcFmt = AV_PIX_FMT_NONE;

    auto makeScaler = [&](int w, int h, AVPixelFormat f) -> bool {
        if (sws && w == srcW && h == srcH && f == srcFmt)
            return true;
        sws.reset(sws_getContext(w, h, f, kOutW, kOutH, kOutPix,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws) {
            spdlog::error("[decode] sws_getContext failed");
            return false;
        }
        srcW = w;
        srcH = h;
        srcFmt = f;
        return true;
    };

    // Pre‑allocated frames & buffers
    FramePtr dst(av_frame_alloc(), &free_frame);
    FramePtr frm(av_frame_alloc(), &free_frame);
    FramePtr sw(av_frame_alloc(), &free_frame);
    PacketPtr pkt(av_packet_alloc(), &free_packet);

    std::vector<uint8_t> gray(kOutW * kOutH);
    av_image_fill_arrays(dst->data, dst->linesize, gray.data(),
        kOutPix, kOutW, kOutH, 1);
    dst->width = kOutW;
    dst->height = kOutH;
    dst->format = kOutPix;

    auto pushFrame = [&] {
        CImg<float> img(kOutW, kOutH, 1, 1);
        for (int y = 0; y < kOutH; ++y) {
            uint8_t const* s = dst->data[0] + y * dst->linesize[0];
            float* o = img.data(0, y);
            for (int x = 0; x < kOutW; ++x)
                o[x] = s[x] / 255.f;
        }
        frames.emplace_back(std::move(img));
    };

    // Error tracking
    int consecutiveErr = 0, totalErr = 0;
    auto tooManyErrors = [&] {
        return consecutiveErr >= kMaxConsecutiveErrors || totalErr >= kMaxTotalErrors;
    };

    // Drain helper: receive‑frame loop
    auto drain = [&]() -> bool {
        while ((ret = avcodec_receive_frame(codecCtx.get(), frm.get())) >= 0) {

            // Detect silent SW fallback and fix log
            if (!really_used_hw(frm.get()) && backendUsed == DecodeBackend::Hardware) {
                backendUsed = DecodeBackend::Software;
                // bump threads now that we know we’re on CPU
                if (codecCtx->thread_count <= 1) {
                    codecCtx->thread_count = std::max(1u, std::thread::hardware_concurrency());
                }
            }
            log_backend(); // prints only once

            // reset consecutive errors on *successful push*
            consecutiveErr = 0;

            // time‑window filtering
            int64_t pts = frm->best_effort_timestamp;
            if (pts < startPts || pts > endPts || pts < nextCapture) {
                av_frame_unref(frm.get());
                continue;
            }
            nextCapture += stepPts;

            // choose frame to scale
            AVFrame* use = frm.get();
            if (really_used_hw(frm.get())) {
                if (av_hwframe_transfer_data(sw.get(), frm.get(), 0) < 0) {
                    spdlog::warn("[decode] hwframe_transfer_data failed");
                    av_frame_unref(frm.get());
                    continue;
                }
                use = sw.get();
            }

            if (!makeScaler(use->width, use->height,
                    static_cast<AVPixelFormat>(use->format))
                || sws_scale(sws.get(), use->data, use->linesize, 0, use->height,
                       dst->data, dst->linesize)
                    <= 0) {
                spdlog::warn("[decode] sws_scale failed");
                av_frame_unref(frm.get());
                continue;
            }
            pushFrame();
            av_frame_unref(frm.get());

            if (max_frames > 0 && static_cast<int>(frames.size()) >= max_frames)
                return true;
        }

        if (ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            ++consecutiveErr;
            ++totalErr;
            spdlog::warn("[decode] avcodec_receive_frame: {}", av_err2str_wrap(ret));
        }
        return tooManyErrors();
    };

    // Main read/decode loop
    while (!tooManyErrors() && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt.get());
            continue;
        }

        ret = avcodec_send_packet(codecCtx.get(), pkt.get());
        av_packet_unref(pkt.get());

        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            ++consecutiveErr;
            ++totalErr;
            spdlog::warn("[decode] avcodec_send_packet: {}", av_err2str_wrap(ret));
            continue;
        }
        consecutiveErr = 0;

        if (drain())
            break;
    }

    // Flush decoder
    avcodec_send_packet(codecCtx.get(), nullptr);
    drain();

    if (tooManyErrors()) {
        spdlog::error("[decode] Aborted '{}': exceeded error budget ({} total, {} consecutive)",
            file_path, totalErr, consecutiveErr);
    }
    return frames;
}

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
    CodecCtxPtr codecCtx(avcodec_alloc_context3(codec), &free_codec_ctx);
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
    int64_t const oneSecondPts = static_cast<int64_t>(1.0 / av_q2d(tbase));

    auto try_seek = [&](int flags) {
        return avformat_seek_file(fmtCtx.get(), videoStreamIdx,
                   0, oneSecondPts, oneSecondPts, flags)
            >= 0;
    };

    if (!try_seek(AVSEEK_FLAG_BACKWARD))
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

    sws_setColorspaceDetails(swsCtx.get(),
        sws_getCoefficients(SWS_CS_DEFAULT),
        codecCtx->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
        sws_getCoefficients(SWS_CS_DEFAULT),
        0, 0, 1, 1);

    // Allocate frames and packet
    FramePtr srcFrame(av_frame_alloc(), &free_frame);
    FramePtr rgbFrame(av_frame_alloc(), &free_frame);
    PacketPtr pkt(av_packet_alloc(), &free_packet);

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
            for (int y = 0; y < kThumbH; ++y) {
                std::memcpy(img.scanLine(y),
                    rgbFrame->data[0] + y * rgbFrame->linesize[0],
                    kThumbW * 3);
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
