// SlowVideoProcessor.cpp
#include "SlowVideoProcessor.h"
#include "Hash.h"

#include <QFileInfo>
#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "VideoProcessingUtils.h"
using namespace vpu;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {
constexpr double kSamplePeriodSec = 1.0; // 1 FPS
constexpr int kProbeSize = 10 * 1024 * 1024;
constexpr int kAnalyzeUsec = 10 * 1'000'000;
// constexpr int kOutW = 32;
// constexpr int kOutH = 32;
}

// ── public API ──────────────────────────────────────────────────────────────
std::vector<std::uint64_t>
SlowVideoProcessor::decodeAndHash(
    VideoInfo const& info,
    SearchSettings const& cfg,
    std::function<void(int)> const& onProgress)
{
    // ── validation ─────────────────────────────────────────────────────────
    if (info.path.empty() || !std::filesystem::exists(info.path)) {
        spdlog::error("[slow] invalid path '{}'", info.path);
        notify(onProgress, 100);
        return {};
    }
    if (cfg.slowHash.maxFrames <= 0)
        return {};

    double const skipPct = std::clamp(cfg.slowHash.skipPercent / 100.0, 0.0, 0.40);
    bool const needStop = skipPct > 0.0;

    static std::once_flag ffInit;
    std::call_once(ffInit, [] { av_log_set_level(AV_LOG_WARNING); });

    // ── demux – open & probe ───────────────────────────────────────────────
    FmtPtr fmt;
    {
        AVDictionary* opts = nullptr;
        av_dict_set_int(&opts, "probesize", kProbeSize, 0);
        av_dict_set_int(&opts, "analyzeduration", kAnalyzeUsec, 0);

        AVFormatContext* raw = nullptr;
        if (int e = avformat_open_input(&raw, info.path.c_str(), nullptr, &opts); e < 0) {
            spdlog::error("[ff] avformat_open_input: {}", err2str(e));
            notify(onProgress, 100);
            av_dict_free(&opts);
            return {};
        }
        fmt.reset(raw);
        av_dict_free(&opts);
    }

    if (int e = avformat_find_stream_info(fmt.get(), nullptr); e < 0) {
        spdlog::error("[ff] stream_info: {}", err2str(e));
        notify(onProgress, 100);
        return {};
    }

    int const vStream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vStream < 0) {
        spdlog::warn("[ff] no video stream found");
        notify(onProgress, 100);
        return {};
    }
    AVStream* st = fmt->streams[vStream];

    // ── decoder ────────────────────────────────────────────────────────────
    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        spdlog::warn("[ff] unsupported codec {}");
        notify(onProgress, 100);
        return {};
    }

    CtxPtr decCtx { avcodec_alloc_context3(dec) };
    if (!decCtx || avcodec_parameters_to_context(decCtx.get(), st->codecpar) < 0) {
        spdlog::error("[ff] parameters_to_context failed");
        notify(onProgress, 100);
        return {};
    }

    decCtx->thread_type = FF_THREAD_SLICE;
    decCtx->thread_count = std::max(1u, std::thread::hardware_concurrency());
    decCtx->skip_frame = AVDISCARD_DEFAULT;   // keep refs & timestamps stable
    decCtx->skip_loop_filter = AVDISCARD_ALL; // perf – still visually fine for hashing
    decCtx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (int e = avcodec_open2(decCtx.get(), dec, nullptr); e < 0) {
        spdlog::error("[ff] avcodec_open2: {}", err2str(e));
        notify(onProgress, 100);
        return {};
    }

    // ── seek setup ─────────────────────────────────────────────────────────
    int64_t const stepPts = sec_to_pts(kSamplePeriodSec, st->time_base);
    int64_t nextPts = 0;

    auto performSeek = [&](double pct) {
        if (info.duration <= 0)
            return false;
        int64_t targetPts = sec_to_pts(pct * info.duration, st->time_base);
        if (av_seek_frame(fmt.get(), vStream, targetPts, AVSEEK_FLAG_BACKWARD) < 0)
            return false;
        avcodec_flush_buffers(decCtx.get());
        // avformat_flush(fmt.get());
        nextPts = targetPts; // capture first sample immediately
        spdlog::info("[seek] → {:.1f}%", pct * 100.0);
        return true;
    };

    if (!performSeek(skipPct))
        spdlog::info("[seek] skip disabled or failed – decoding from start");

    // pts at which we must stop (just before final skipPct%)
    int64_t const ptsEnd = needStop && info.duration > 0
        ? sec_to_pts((1.0 - skipPct) * info.duration, st->time_base)
        : INT64_MAX;

    // ── buffers ────────────────────────────────────────────────────────────
    std::vector<uint8_t> grayBuf; // scratch for hash_frame()
    FrmPtr frame { av_frame_alloc() };
    PktPtr pkt { av_packet_alloc() };
    if (!frame || !pkt) {
        spdlog::error("[ff] allocation failure");
        notify(onProgress, 100);
        return {};
    }

    std::vector<std::uint64_t> hashes;
    hashes.reserve(cfg.slowHash.maxFrames);

    // --- decoding loop ---
    bool fatal = false;
    while (!fatal && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != vStream) {
            av_packet_unref(pkt.get());
            continue;
        }

        // handle EAGAIN internally – feed packets until decoder accepts one
        for (;;) {
            int s = avcodec_send_packet(decCtx.get(), pkt.get());
            if (s == 0) {
                av_packet_unref(pkt.get());
            } // stole packet
            else if (s == AVERROR(EAGAIN)) { // need to recv
            } else if (s == AVERROR_EOF) {
                av_packet_unref(pkt.get());
                break;
            } else {
                spdlog::warn("[ff] send_packet: {}", err2str(s));
                fatal = true;
                break;
            }

            // pull as many frames as decoder emits
            while (!fatal) {
                int r = avcodec_receive_frame(decCtx.get(), frame.get());
                if (r == AVERROR(EAGAIN))
                    break; // need more input
                if (r == AVERROR_EOF)
                    goto decodeDone;
                if (r < 0) {
                    spdlog::warn("[ff] receive_frame: {}", err2str(r));
                    fatal = true;
                    break;
                }

                int64_t const pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts : frame->best_effort_timestamp;
                if (pts >= ptsEnd)
                    goto decodeDone; // passed region of interest
                if (sample_due(pts, nextPts)) {
                    if (auto hval = hash_frame(frame.get(), grayBuf, fatal)) {
                        hashes.push_back(*hval);
                        notify(onProgress,
                            static_cast<int>(hashes.size() * 100 / cfg.slowHash.maxFrames));
                    }
                    nextPts += stepPts;
                    if (hashes.size() >= static_cast<std::size_t>(cfg.slowHash.maxFrames))
                        goto decodeDone;
                }
                av_frame_unref(frame.get());
            }
            if (s != AVERROR(EAGAIN))
                break; // packet done
        }
    }

decodeDone:
    // ── flush remaining ────────────────────────────────────────────────────
    if (!fatal && hashes.size() < static_cast<std::size_t>(cfg.slowHash.maxFrames)) {
        avcodec_send_packet(decCtx.get(), nullptr); // signal EOF
        while (true) {
            int r = avcodec_receive_frame(decCtx.get(), frame.get());
            if (r == AVERROR_EOF || r == AVERROR(EAGAIN))
                break;
            if (r < 0) {
                spdlog::warn("[ff] flush receive_frame: {}", err2str(r));
                break;
            }

            int64_t const pts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts : frame->best_effort_timestamp;
            if (pts >= ptsEnd)
                break;
            if (sample_due(pts, nextPts)) {
                if (auto hval = hash_frame(frame.get(), grayBuf, fatal)) {
                    hashes.push_back(*hval);
                    notify(onProgress,
                        static_cast<int>(hashes.size() * 100 / cfg.slowHash.maxFrames));
                }
                nextPts += stepPts;
                if (hashes.size() >= static_cast<std::size_t>(cfg.slowHash.maxFrames))
                    break;
            }
            av_frame_unref(frame.get());
        }
    }

    spdlog::info("[slow] done – {} hashes generated ({} requested)", hashes.size(), cfg.slowHash.maxFrames);
    notify(onProgress, 100);
    return hashes;
}
