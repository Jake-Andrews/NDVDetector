#include "SlowVideoProcessor.h"
#include "Hash.h"
#include "VideoProcessingUtils.h"

namespace {
constexpr double kSamplePeriodSec = 1.0; // Sample 1 frame per second
}
#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// Error checking macro for FFmpeg calls
#define AVCHECK(expr)                                                  \
    do {                                                               \
        int _ret = (expr);                                             \
        if (_ret < 0) {                                                \
            spdlog::error("{} failed: {}", #expr, vpu::err2str(_ret)); \
            throw std::runtime_error("FFmpeg call failed");            \
        }                                                              \
    } while (false)

// HashPool implementation
HashPool::HashPool(std::size_t nWorkers, BoundedQueue<FrmPtr>& q,
    std::vector<uint64_t>& outHashes, std::atomic_bool& fatal)
    : q_(q)
    , hashes_(outHashes)
    , fatal_(fatal)
{
    for (std::size_t i = 0; i < nWorkers; ++i) {
        workers_.emplace_back([this](std::stop_token tk) { worker_loop(tk); });
    }
}

HashPool::~HashPool() = default;

void HashPool::worker_loop(std::stop_token tk)
{
    thread_local std::vector<uint8_t> scratch;

    while (!tk.stop_requested()) {
        FrmPtr frame;
        if (!q_.pop(frame, tk))
            break; // cancelled
        if (!frame)
            break; // poison pill

        bool local_fatal = false;
        if (auto h = vpu::hash_frame(frame.get(), scratch, local_fatal)) {
            hashes_.push_back(*h);
        }
        if (local_fatal) {
            fatal_.store(true, std::memory_order_relaxed);
            break;
        }
    }
}

std::vector<uint64_t>
SlowVideoProcessor::decodeAndHash(VideoInfo const& info, SearchSettings const& cfg)
{
    if (info.path.empty()) {
        spdlog::warn("[hasher] Empty path");
        return {};
    }

    constexpr std::size_t kQueueCap = 64;
    BoundedQueue<FrmPtr> frameQ { kQueueCap };
    std::vector<uint64_t> hashes;
    hashes.reserve(cfg.slowHash.maxFrames);
    std::atomic_bool fatal { false };

    // Hash workers
    std::size_t const poolSize = std::max(1u, std::thread::hardware_concurrency() - 2u);
    HashPool pool { poolSize, frameQ, hashes, fatal };

    // Demux + decode thread
    std::jthread ddThr([&](std::stop_token tk) {
        try {
            demux_decode_loop(info, cfg, tk, frameQ, fatal);
        } catch (std::exception const& e) {
            spdlog::error("[hasher] demux/decode fatal: {}", e.what());
            fatal = true;
        }
    });

    // Wait for demux thread to finish then stop workers
    ddThr.join();
    for (std::size_t i = 0; i < poolSize; ++i)
        frameQ.push(nullptr, std::stop_token {}); // poison pills

    if (fatal) {
        spdlog::error("[hasher] Aborted – {} hashes produced", hashes.size());
        hashes.clear();
    }
    return hashes;
}

void SlowVideoProcessor::demux_decode_loop(VideoInfo const& info,
    SearchSettings const& cfg,
    std::stop_token tk,
    BoundedQueue<FrmPtr>& frameQ,
    std::atomic_bool& fatal)
{
    // Open container
    FmtPtr fmt;
    {
        AVDictionary* opts = nullptr;
        av_dict_set_int(&opts, "probesize", 10 * 1024 * 1024, 0);
        av_dict_set_int(&opts, "analyzeduration", 10'000'000, 0);
        AVFormatContext* raw = nullptr;
        AVCHECK(avformat_open_input(&raw, info.path.c_str(), nullptr, &opts));
        fmt.reset(raw);
        av_dict_free(&opts);
    }
    AVCHECK(avformat_find_stream_info(fmt.get(), nullptr));

    int vStream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vStream < 0)
        throw std::runtime_error("No video stream");
    AVStream* st = fmt->streams[vStream];

    // Setup PTS tracking
    int64_t const stepPts = vpu::sec_to_pts(kSamplePeriodSec, st->time_base);
    int64_t nextPts = 0;

    // Open decoder
    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec)
        throw std::runtime_error("Unsupported codec");

    CtxPtr decCtx { avcodec_alloc_context3(dec) };
    AVCHECK(avcodec_parameters_to_context(decCtx.get(), st->codecpar));

    decCtx->thread_type = (dec->capabilities & AV_CODEC_CAP_FRAME_THREADS) ? FF_THREAD_FRAME
        : (dec->capabilities & AV_CODEC_CAP_SLICE_THREADS)                 ? FF_THREAD_SLICE
                                                                           : 0;
    decCtx->thread_count = 0; // auto

    if (cfg.slowHash.useKeyframesOnly) {
        decCtx->skip_frame = AVDISCARD_NONKEY;
        decCtx->skip_idct = AVDISCARD_NONKEY;
    }
    decCtx->skip_loop_filter = AVDISCARD_ALL;
    decCtx->flags2 |= AV_CODEC_FLAG2_FAST;

    AVCHECK(avcodec_open2(decCtx.get(), dec, nullptr));
    spdlog::info("Decoder threads {} (mode = {})", decCtx->thread_count,
        decCtx->thread_type == FF_THREAD_FRAME       ? "frame"
            : decCtx->thread_type == FF_THREAD_SLICE ? "slice"
                                                     : "none");

    // Main decode loop
    PktPtr pkt { av_packet_alloc() };
    FrmPtr frm { av_frame_alloc() };

    auto send_pkt = [&](AVPacket* p) {
        int s = avcodec_send_packet(decCtx.get(), p);
        if (s == AVERROR(EAGAIN))
            return;
        AVCHECK(s);
    };

    auto receive_frames = [&] {
        for (;;) {
            int r = avcodec_receive_frame(decCtx.get(), frm.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            AVCHECK(r);

            // Get frame PTS
            int64_t pts = (frm.get()->pts != AV_NOPTS_VALUE)
                ? frm.get()->pts
                : frm.get()->best_effort_timestamp;

            // Only process frame if it's time for a sample
            if (vpu::sample_due(pts, nextPts)) {
                FrmPtr clone { av_frame_clone(frm.get()) };
                if (!frameQ.push(std::move(clone), tk)) {
                    fatal = true;
                    return;
                }
                nextPts += stepPts;
            }
            av_frame_unref(frm.get());
        }
    };

    while (!tk.stop_requested() && !fatal) {
        if (av_read_frame(fmt.get(), pkt.get()) >= 0) {
            if (pkt->stream_index == vStream)
                send_pkt(pkt.get());
            av_packet_unref(pkt.get());
        } else {
            send_pkt(nullptr);
            receive_frames();
            break;
        }
        receive_frames();
    }
}
