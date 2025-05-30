// SlowVideoProcessor.cpp
#include "SlowVideoProcessor.h"
#include "Hash.h"
#include "VideoProcessingUtils.h"
using namespace vpu;

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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {
constexpr double kSamplePeriodSec = 1.0;
constexpr int kProbeSize = 10 * 1024 * 1024;
constexpr int kAnalyzeUsec = 10 * 1'000'000;
}

// --- RAII & decoder-drain helpers ---
namespace {

struct FrameRAII {
    AVFrame* f { av_frame_alloc() };
    FrameRAII() = default;
    FrameRAII(FrameRAII&& o) noexcept
        : f(o.f)
    {
        o.f = nullptr;
    }
    FrameRAII& operator=(FrameRAII&& o) noexcept
    {
        if (this != &o) {
            av_frame_free(&f);
            f = o.f;
            o.f = nullptr;
        }
        return *this;
    }
    ~FrameRAII() { av_frame_free(&f); }
    AVFrame* get() const noexcept { return f; }
    void unref() noexcept
    {
        if (f)
            av_frame_unref(f);
    }
};

// unique_ptr alias using the generic deleter from VideoProcessingUtils.h
// using PktPtr = std::unique_ptr<AVPacket, vpu::CDeleter<&av_packet_free>>;

// Pull every ready frame once; return true if a frame was consumed, false otherwise
template<class OnHash>
bool drain_decoder_once(AVCodecContext* ctx,
    FrameRAII& frm,
    int64_t ptsEnd,
    int64_t& nextPts,
    int64_t stepPts,
    bool& fatal,
    bool& done,
    OnHash&& onHash)
{
    while (true) {
        int r = avcodec_receive_frame(ctx, frm.get());
        if (r == AVERROR(EAGAIN))
            return false; // no frame produced
        if (r == AVERROR_EOF)
            return false; // fully drained
        if (r < 0) {      // fatal error
            fatal = true;
            frm.unref();
            return false;
        }

        int64_t pts = (frm.get()->pts != AV_NOPTS_VALUE)
            ? frm.get()->pts
            : frm.get()->best_effort_timestamp;

        if (pts >= ptsEnd) {
            frm.unref();  // avoid leak
            done = true;  // tell caller to stop feeding
            return false; // stop decoding
        }

        if (vpu::sample_due(pts, nextPts)) {
            onHash(frm.get());
            nextPts += stepPts;
        }
        frm.unref(); // normal case
        return true; // consumed 1 frame
    }
}

} // anonymous namespace

std::vector<std::uint64_t>
SlowVideoProcessor::decodeAndHash(
    VideoInfo const& info,
    SearchSettings const& cfg,
    std::function<void(int)> const& onProgress)
{
    // --- validation ---
    if (info.path.empty() || !std::filesystem::exists(info.path)) {
        spdlog::error("[slow] invalid path '{}'", info.path);
        return {};
    }
    if (cfg.slowHash.maxFrames <= 0)
        return {};

    double const skipPct = std::clamp(cfg.slowHash.skipPercent / 100.0, 0.0, 0.40);
    bool const needStop = skipPct > 0.0;

    static std::once_flag ffInit;
    std::call_once(ffInit, [] { av_log_set_level(AV_LOG_WARNING); });

    // --- demux - open & probe ---
    FmtPtr fmt;
    {
        AVDictionary* opts = nullptr;
        av_dict_set_int(&opts, "probesize", kProbeSize, 0);
        av_dict_set_int(&opts, "analyzeduration", kAnalyzeUsec, 0);

        AVFormatContext* raw = nullptr;
        if (int e = avformat_open_input(&raw, info.path.c_str(), nullptr, &opts); e < 0) {
            spdlog::error("[ff] avformat_open_input: {}", err2str(e));
            av_dict_free(&opts);
            return {};
        }
        fmt.reset(raw);
        av_dict_free(&opts);
    }

    // --- stream ---
    if (int e = avformat_find_stream_info(fmt.get(), nullptr); e < 0) {
        spdlog::error("[ff] stream_info: {}", err2str(e));
        return {};
    }

    int const vStream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vStream < 0) {
        spdlog::warn("[ff] no video stream found");
        return {};
    }
    AVStream* st = fmt->streams[vStream];

    // --- decoder ---
    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        spdlog::warn("[ff] unsupported codec {}");
        return {};
    }

    CtxPtr decCtx { avcodec_alloc_context3(dec) };
    if (!decCtx || avcodec_parameters_to_context(decCtx.get(), st->codecpar) < 0) {
        spdlog::error("[ff] parameters_to_context failed");
        return {};
    }

    // choose best threading mode supported by this codec
    int tt = 0;
    if (dec->capabilities & AV_CODEC_CAP_FRAME_THREADS) // prefer frame-threads
        tt = FF_THREAD_FRAME;
    else if (dec->capabilities & AV_CODEC_CAP_SLICE_THREADS) // else fall back to slice-threads
        tt = FF_THREAD_SLICE;
    decCtx->thread_type = tt;
    decCtx->thread_count = std::clamp<unsigned>(std::thread::hardware_concurrency(), 1, 16);
    decCtx->skip_frame = AVDISCARD_DEFAULT;
    // disable deblocking filter for faster decoding
    decCtx->skip_loop_filter = AVDISCARD_ALL;
    // disable strict compliance checks, prioritize speed
    decCtx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (int e = avcodec_open2(decCtx.get(), dec, nullptr); e < 0) {
        spdlog::error("[ff] avcodec_open2: {}", err2str(e));
        return {};
    }

    // --- seek setup ---
    int64_t const stepPts = sec_to_pts(kSamplePeriodSec, st->time_base);
    int64_t nextPts = 0;

    // convert percentage to PCT and seek backwards to a keyframe
    auto performSeek = [&](double pct) {
        if (info.duration <= 0)
            return false;
        nextPts = sec_to_pts(pct * info.duration, st->time_base);
        if (av_seek_frame(fmt.get(), vStream, nextPts, AVSEEK_FLAG_BACKWARD) < 0)
            return false;
        avcodec_flush_buffers(decCtx.get());
        avformat_flush(fmt.get());
        spdlog::info("[seek] → {:.1f}%", pct * 100.0);
        return true;
    };

    if (!performSeek(skipPct)) {
        spdlog::warn("[seek] skip disabled or failed, skipping: {}", info.path);
        return {};
    }

    // pts at which we must stop to skip the last %
    int64_t const ptsEnd = needStop && info.duration > 0
        ? sec_to_pts((1.0 - skipPct) * info.duration, st->time_base)
        : INT64_MAX;

    // --- buffers ---
    std::vector<uint8_t> grayBuf; // scratch for hash_frame()
    FrameRAII frame;
    PktPtr pkt { av_packet_alloc() };
    if (!frame.get() || !pkt) {
        spdlog::error("[ff] allocation failure, skipping: {}", info.path);
        return {};
    }

    std::vector<std::uint64_t> hashes;
    hashes.reserve(cfg.slowHash.maxFrames);

    bool fatal = false;
    bool done = false;

    auto pushHash = [&](AVFrame* frm) {
        if (auto h = hash_frame(frm, grayBuf, fatal)) {
            hashes.push_back(*h);
        }
    };

    auto discardHash = [](AVFrame*) { /* no-op – just drop the frame */ };

    // --- decoding loop ---
    while (!fatal && !done && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != vStream) {
            av_packet_unref(pkt.get());
            continue;
        }

        while (!fatal) {
            int s = avcodec_send_packet(decCtx.get(), pkt.get());
            if (s == 0) {
                av_packet_unref(pkt.get());    // pkt consumed
            } else if (s == AVERROR(EAGAIN)) { /* need to drain first */
            } else {
                spdlog::warn("[ff] send_packet: {}",
                    vpu::err2str(s));
                fatal = true;
                break;
            }

            while (drain_decoder_once(decCtx.get(), frame,
                ptsEnd, nextPts, stepPts,
                fatal, done, pushHash)) { }

            if (hashes.size() >= std::size_t(cfg.slowHash.maxFrames))
                done = true;

            if (s != AVERROR(EAGAIN))
                break; // packet fully accepted
        }
        av_packet_unref(pkt.get());
    }

    // mini-drain: empty decoder queue that already existed when we stopped
    if (!fatal) {
        while (drain_decoder_once(decCtx.get(), frame,
            std::numeric_limits<int64_t>::max(), // ignore ptsEnd
            nextPts, stepPts,
            fatal, done, discardHash)) {
        }
    }

    // final flush if we still need hashes
    if (!fatal && hashes.size() < std::size_t(cfg.slowHash.maxFrames)) {
        avcodec_send_packet(decCtx.get(), nullptr); // signal EOF
        while (drain_decoder_once(decCtx.get(), frame,
            std::numeric_limits<int64_t>::max(),
            nextPts, stepPts,
            fatal, done, discardHash)) { }
    }

    spdlog::info("[slow] done – {} hashes generated ({} requested)", hashes.size(), cfg.slowHash.maxFrames);
    return hashes;
}
