// FastVideoProcessor.cpp
#include "FastVideoProcessor.h"
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
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
    void unref() const noexcept
    {
        if (f)
            av_frame_unref(f);
    }
};

} // namespace

static constexpr int KPROBESIZE = 10 * 1024 * 1024;
static constexpr int KANALYZEUSEC = 10 * 1'000'000;

// Because we have to seek to a keyframe, the keyframe is likely not at the
// desired timestamp. therefore this function decodes from the keyframe until
// the frame that is at the desired timestamp to ensure consistent pHashes
// across videos regardless of keyframe placement
static std::optional<uint64_t> decode_until_timestamp(
    AVFormatContext* fmt,
    AVCodecContext* codec_ctx,
    int vstream,
    int64_t target_pts,
    FrameRAII& frame,
    PktPtr& pkt,
    std::vector<uint8_t>& grayBuf,
    bool& fatal_error)
{
    while (!fatal_error && av_read_frame(fmt, pkt.get()) >= 0) {
        if (pkt->stream_index != vstream) {
            av_packet_unref(pkt.get());
            continue;
        }

        int s;
        while ((s = avcodec_send_packet(codec_ctx, pkt.get())) == AVERROR(EAGAIN)) {
            AVFrame* tmp = frame.get();
            while (avcodec_receive_frame(codec_ctx, tmp) >= 0)
                av_frame_unref(tmp);
        }

        if (s < 0) {
            av_packet_unref(pkt.get());
            fatal_error = true;
            break;
        }
        av_packet_unref(pkt.get());

        while (avcodec_receive_frame(codec_ctx, frame.get()) >= 0) {
            int64_t pts = (frame.get()->pts != AV_NOPTS_VALUE)
                ? frame.get()->pts
                : frame.get()->best_effort_timestamp;

            if (pts >= target_pts) {
                auto hash = hash_frame(frame.get(), grayBuf, fatal_error);
                frame.unref();
                return hash;
            }
            frame.unref();
        }
    }
    // After reading all packets, flush the decoder once
    avcodec_send_packet(codec_ctx, nullptr); // flush
    while (avcodec_receive_frame(codec_ctx, frame.get()) >= 0) {
        int64_t pts = (frame.get()->pts != AV_NOPTS_VALUE)
            ? frame.get()->pts
            : frame.get()->best_effort_timestamp;
        if (pts >= target_pts) {
            auto hash = hash_frame(frame.get(), grayBuf, fatal_error);
            frame.unref();
            return hash;
        }
        frame.unref();
    }
    if (!fatal_error) // reached EOF but never hit target
        fatal_error = true;
    return std::nullopt;
}

// saves frames to fs for debugging
/*
static constexpr bool KDUMPFRAMES = false;
static void write_pgm(std::filesystem::path const& path,
    uint8_t const* data, int w, int h, int stride)
{
    std::ofstream os(path, std::ios::binary);
    if (!os)
        return;
    os << "P5\n"
       << w << ' ' << h << "\n255\n";
    for (int y = 0; y < h; ++y)
        os.write(reinterpret_cast<char const*>(data + y * stride), w);
}
*/

std::vector<std::uint64_t>
FastVideoProcessor::decodeAndHash(
    VideoInfo const& v,
    SearchSettings const& cfg)
{
    // --- fail fast on non-sense input ---
    if (v.path.empty() || !std::filesystem::exists(v.path)) {
        throw std::runtime_error(std::format(
            "Invalid configuration v.path: '{}', must not be empty",
            v.path));
    }
    if (cfg.fastHash.maxFrames <= 0)
        throw std::runtime_error(std::format(
            "Invalid configuration cfg.fastHash.maxFrames: '{}', must be > 0",
            cfg.fastHash.maxFrames));

    static std::once_flag ffOnce;
    std::call_once(ffOnce, [] { av_log_set_level(AV_LOG_WARNING); });

    // --- open demuxer ---
    FmtPtr fmt;
    {
        AVDictionary* o = nullptr;
        av_dict_set_int(&o, "probesize", KPROBESIZE, 0);
        av_dict_set_int(&o, "analyzeduration", KANALYZEUSEC, 0);
        AVFormatContext* raw = nullptr;
        int e = avformat_open_input(&raw, v.path.c_str(), nullptr, &o);
        av_dict_free(&o);
        if (e < 0) {
            spdlog::error("[ffmpeg-sw] avformat_open_input: {}", ff_err2str(e));
            return {};
        }
        fmt.reset(raw);
    }
    if (int e = avformat_find_stream_info(fmt.get(), nullptr); e < 0) {
        spdlog::error("[ffmpeg-sw] stream_info: {}", ff_err2str(e));
        return {};
    }

    int vstream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream < 0) {
        spdlog::warn("[ffmpeg-sw] no video stream");
        return {};
    }
    AVStream* st = fmt->streams[vstream];

    // --- decoder open ---
    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        spdlog::warn("[ffmpeg-sw] unsupported codec ID {}", static_cast<int>(st->codecpar->codec_id));
        return {};
    }

    CtxPtr codec_ctx { avcodec_alloc_context3(dec) };
    if (!codec_ctx || avcodec_parameters_to_context(codec_ctx.get(), st->codecpar) < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_parameters_to_context failed");
        return {};
    }

    // files normally cary only one slice per frame, so FF_THREAD_SLICE provides no parallelism
    codec_ctx->thread_type = FF_THREAD_FRAME;

    // bool intra_only = st->codecpar->codec_id == AV_CODEC_ID_PRORES || st->codecpar->codec_id == AV_CODEC_ID_MJPEG || st->codecpar->codec_id == AV_CODEC_ID_JPEG2000 || st->codecpar->codec_id == AV_CODEC_ID_VP8;

    //**AVDISCARD_NONREF would speed this up but in the
    // case of long GOP it will be off by quite a bit
    codec_ctx->skip_frame = AVDISCARD_DEFAULT;
    // Let FFmpeg apply codec-specific fast flags
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    // Pick a sane explicit thread count
    unsigned tc = std::thread::hardware_concurrency();
    unsigned requested_threads = tc ? tc : 1; // enforce ≥1
    codec_ctx->thread_count = requested_threads;
    codec_ctx->skip_loop_filter = AVDISCARD_ALL;

    spdlog::info(">>> about to open decoder");
    int err = avcodec_open2(codec_ctx.get(), dec, nullptr);
    if (err < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_open2 failed: {}", ff_err2str(err));
        return {};
    }
    // Log actual thread count after decoder is opened since it may differ
    spdlog::info("[sw] Requested {} threads, using {} for SW decoding",
        requested_threads, codec_ctx->thread_count);
    spdlog::info(">>> decoder opened, err: {}", ff_err2str(err));

    // seek to an exact pts and flush codec
    auto seek_to_pts = [&](int64_t pts, bool keyframeOnly) -> bool {
        if (pts > st->duration) {
            spdlog::warn("[seek] target pts {} exceeds stream duration {}",
                pts, st->duration);
            return false;
        }

        int flags = keyframeOnly ? AVSEEK_FLAG_ANY : AVSEEK_FLAG_BACKWARD;
        if (av_seek_frame(fmt.get(), vstream, pts, flags) < 0) {
            spdlog::warn("[seek] failed to {:.1f}s", pts * av_q2d(st->time_base));
            return false;
        }
        avcodec_flush_buffers(codec_ctx.get());
        avformat_flush(fmt.get());
        return true;
    };

    // --- pre‑allocate frames/packets ---
    FrameRAII frame;
    PktPtr pkt { av_packet_alloc() };

    std::vector<uint8_t> grayBuf; // scratch for full-res GRAY8 frames

    /*
    std::filesystem::path dumpDir;
    if constexpr (KDUMPFRAMES) {
        dumpDir = std::filesystem::path("frames") / QFileInfo(QString::fromStdString(v.path)).completeBaseName().toStdString();
        std::filesystem::create_directories(dumpDir);
    }
    */
    bool fatal_error = false;

    // --- main decode / hash loop : exactly 2 hashes ---
    std::vector<uint64_t> hashes;
    hashes.reserve(2);
    std::array<double, 2> const targetsPct = { 0.30, 0.70 };

    for (double pct : targetsPct) {
        if (v.duration <= 0) {
            fatal_error = true;
            break;
        }

        int64_t target_pts = sec_to_pts(pct * v.duration, st->time_base);
        if (!seek_to_pts(target_pts, cfg.fastHash.useKeyframesOnly)) {
            fatal_error = true;
            break;
        }

        std::optional<uint64_t> hash;
        if (cfg.fastHash.useKeyframesOnly) {
            // In keyframe mode, try to use the keyframe directly first
            AVFrame* tmp = frame.get();
            int rc = avcodec_receive_frame(codec_ctx.get(), tmp);
            if (rc >= 0) {
                // We got a keyframe directly
                hash = hash_frame(tmp, grayBuf, fatal_error);
                frame.unref();
            } else {
                // Need to decode at least one frame
                hash = decode_until_timestamp(fmt.get(), codec_ctx.get(), vstream,
                    std::numeric_limits<int64_t>::min(), // accept first decoded frame
                    frame, pkt, grayBuf, fatal_error);
            }
        } else {
            hash = decode_until_timestamp(fmt.get(), codec_ctx.get(), vstream,
                target_pts, frame, pkt, grayBuf, fatal_error);
        }

        if (!hash) {
            fatal_error = true;
            break;
        }

        hashes.push_back(*hash);
    }

    spdlog::info("[sw] finished: {} hashes generated{}", hashes.size(),
        fatal_error ? " (fatal error)" : "");

    // Only return results if we got exactly the expected number of hashes
    // and no fatal errors occurred
    if (fatal_error || hashes.size() != targetsPct.size()) {
        spdlog::error("[sw] Failed to generate all required hashes");
        return {};
    }
    return hashes;
}
