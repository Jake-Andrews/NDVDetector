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
#include <functional>
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

// pull at most one decoded frame; return true if a frame was consumed
template<class OnFrame>
bool drain_decoder_once(AVCodecContext* ctx,
    FrameRAII& frm,
    int64_t& nextPts,
    bool& fatal,
    OnFrame&& onFrame)
{
    while (true) {
        int r = avcodec_receive_frame(ctx, frm.get());
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return false;
        if (r < 0) {
            fatal = true;
            frm.unref();
            return false;
        }

        onFrame(frm.get());
        frm.unref();
        return true;
    }
}

} // namespace

static constexpr int KPROBESIZE = 10 * 1024 * 1024;
static constexpr int KANALYZEUSEC = 10 * 1'000'000;

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
    SearchSettings const& cfg,
    std::function<void(int)> const& on_progress)
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
    // Pick a sane explicit thread count (0 = FFmpeg default = may become 1)
    // For per frame decoding use 1
    // codec_ctx->thread_count = 0;
    unsigned tc = std::thread::hardware_concurrency();
    codec_ctx->thread_count = tc ? tc : 1; // enforce ≥1
    spdlog::info("[sw] Using {} threads for SW decoding", codec_ctx->thread_count);
    codec_ctx->skip_idct = AVDISCARD_ALL;
    codec_ctx->skip_loop_filter = AVDISCARD_ALL;

    spdlog::info(">>> about to open decoder");
    int err = avcodec_open2(codec_ctx.get(), dec, nullptr);
    if (err < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_open2 failed: {}", ff_err2str(err));
        return {};
    }
    spdlog::info(">>> decoder opened, err: {}", ff_err2str(err));

    /* seek to an exact pts (BACKWARD to nearest key-frame) and flush codec  */
    auto seek_to_pts = [&](int64_t pts) -> bool {
        if (av_seek_frame(fmt.get(), vstream, pts, AVSEEK_FLAG_BACKWARD) < 0) {
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

        int64_t tgtPts = sec_to_pts(pct * v.duration, st->time_base);
        if (!seek_to_pts(tgtPts)) {
            fatal_error = true;
            break;
        }

        uint64_t tgtHash = 0;
        bool gotHash = false;

        while (!fatal_error && av_read_frame(fmt.get(), pkt.get()) >= 0) {
            if (pkt->stream_index != vstream) {
                av_packet_unref(pkt.get());
                continue;
            }

            int s;
            while ((s = avcodec_send_packet(codec_ctx.get(), pkt.get())) == AVERROR(EAGAIN)) {
                /* decoder full – drain to make space */
                AVFrame* tmp = frame.get();
                if (avcodec_receive_frame(codec_ctx.get(), tmp) >= 0)
                    av_frame_unref(tmp);
            }
            if (s < 0) {
                av_packet_unref(pkt.get());
                fatal_error = true;
                break;
            }
            av_packet_unref(pkt.get());

            /* pull every available frame */
            while (avcodec_receive_frame(codec_ctx.get(), frame.get()) >= 0) {
                int64_t pts = (frame.get()->pts != AV_NOPTS_VALUE)
                    ? frame.get()->pts
                    : frame.get()->best_effort_timestamp;

                if (pts >= tgtPts) { // first frame after mark
                    if (auto h = hash_frame(frame.get(), grayBuf, fatal_error)) {
                        tgtHash = *h;
                        gotHash = true;
                    }
                    frame.unref();
                    break; // stop draining
                }

                frame.unref(); // before-mark frame – ignore
            }
            if (gotHash)
                break; // leave av_read_frame loop
        }

        if (!gotHash) { // no frame found before EOF
            fatal_error = true;
            break;
        }

        hashes.push_back(tgtHash);
    }

    spdlog::info("[sw] finished: {} hashes generated", hashes.size());
    return hashes;
}
