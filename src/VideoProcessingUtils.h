#pragma once
#include "Hash.h"
#include <memory>
#include <vector>
#include <cstdint>
#include <functional>
#include <cmath>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/avutil.h>
}

namespace vpu      
{
constexpr uint64_t PHASH_ALL_ONE_COLOUR=0x0000000000000000ULL;

template<auto Fn>
struct CDeleter {
    template<class T> void operator()(T* p) const noexcept { if (p) Fn(&p); }
};

using FmtPtr = std::unique_ptr<AVFormatContext, CDeleter<&avformat_close_input>>;
using CtxPtr = std::unique_ptr<AVCodecContext,   CDeleter<&avcodec_free_context>>;
using FrmPtr = std::unique_ptr<AVFrame,          CDeleter<&av_frame_free>>;
using PktPtr = std::unique_ptr<AVPacket,         CDeleter<&av_packet_free>>;
using BufPtr = std::unique_ptr<AVBufferRef,      CDeleter<&av_buffer_unref>>;

// -----------------------------------------------------------------
// misc helpers  (error-string, timebase scale, progress callback)
// -----------------------------------------------------------------
inline char const* err2str(int e)
{
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    return av_make_error_string(buf, sizeof(buf), e);
}
inline char const* ff_err2str(int e) { return err2str(e); }        

inline int64_t sec_to_pts(double sec, AVRational tb)
{
    assert(tb.num > 0 && tb.den > 0);

    int64_t usec = static_cast<int64_t>(std::llrint(sec * AV_TIME_BASE));
    return av_rescale_q_rnd(usec, AV_TIME_BASE_Q, tb,
                            static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                                    AV_ROUND_PASS_MINMAX));
}

inline void notify(std::function<void(int)> const& cb, int pct)       { if (cb) cb(pct); }
inline void report_progress(std::function<void(int)> const& cb, int p){ if (cb) cb(p); }

// should we sample this frame?
inline bool sample_due(int64_t framePts, int64_t nextPts)
{
    return framePts == AV_NOPTS_VALUE || framePts >= nextPts;
}

// full-resolution Y-plane extractor  (AVFrame → GRAY8 contiguous buf)
inline bool extract_luma_full(AVFrame const* src,
                              std::vector<uint8_t>& dst,
                              int& outW, int& outH)
{
    if (!src) return false;

    outW = src->width;
    outH = src->height;
    dst.resize(static_cast<std::size_t>(outW) * outH);

    static thread_local SwsContext* sws = nullptr;
    static thread_local int lastW = 0, lastH = 0, lastFmt = AV_PIX_FMT_NONE;

    if (!sws || lastW != outW || lastH != outH || lastFmt != src->format) {
        if (sws) sws_freeContext(sws);
        sws = sws_getContext(outW, outH,
                             static_cast<AVPixelFormat>(src->format),
                             outW, outH, AV_PIX_FMT_GRAY8,
                             SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            spdlog::error("[sws] context init failed");
            return false;
        }
        lastW = outW;  lastH = outH;  lastFmt = src->format;
    }

    uint8_t* dstData[1] = { dst.data() };
    int      dstLines[1] = { outW };

    return sws_scale(sws, src->data, src->linesize, 0, outH,
                     dstData, dstLines) > 0;
}

// Convert frame → hash.  Returns std::nullopt on failure.
inline std::optional<uint64_t>
hash_frame(AVFrame const* frm, std::vector<uint8_t>& buf, bool& fatal_error)
{
    try {
        int w = 0, h = 0;
        if (!vpu::extract_luma_full(frm, buf, w, h))
            return std::nullopt;

        /*
        // Early flat frame detection before expensive hash computation
        if (vpu::is_flat_frame(buf.data(), w, h, w)) {
            spdlog::info("[hash] Detected flat frame, generating random hash as placeholder");
            return generate_random_phash();
        }
        */

        // mean-average  → 32×32 down-scale  → Phash
        auto hval = compute_phash_full(buf.data(), w, h);

        if (!hval || *hval == vpu::PHASH_ALL_ONE_COLOUR)
            return std::nullopt;
        return hval;

    } catch (std::exception const& e) {
        spdlog::error("[hash] Fatal error computing hash: {}", e.what());
        fatal_error = true;
        return std::nullopt;
    } catch (...) {
        spdlog::error("[hash] Fatal error computing hash: unknown exception");
        fatal_error = true;
        return std::nullopt;
    }
}

/*
// Detects frames that are:
// 1. Solid color (all sampled pixels identical)
// 2. Nearly black (low mean luminance)
// 3. Low detail (low pixel variance)
inline bool is_flat_frame(uint8_t const* data, int w, int h, int stride)
{
    if (!data || w <= 0 || h <= 0)
        return true;

    static constexpr int GRID = 16;                     // Sample 16×16 grid
    static constexpr double BLACK_THRESHOLD = 16.0;     // Mean below = nearly black
    static constexpr double VARIANCE_THRESHOLD = 25.0;  // Variance below = low detail

    int stepX = std::max(1, w / GRID);
    int stepY = std::max(1, h / GRID);

    double sum = 0.0;
    double sumSq = 0.0;
    int count = 0;

    uint8_t firstVal = data[0];
    bool isSolid = true;

    // Sample full 16×16 grid across frame
    for (int y = 0; y < h; y += stepY) {
        for (int x = 0; x < w; x += stepX) {
            uint8_t val = data[y * stride + x];
            if (isSolid && val != firstVal)
                isSolid = false;

            sum += val;
            sumSq += val * val;
            ++count;
        }
    }

    if (isSolid) {
        spdlog::debug("[frame] Detected solid color frame");
        return true;
    }

    double mean = sum / count;
    double variance = (sumSq / count) - (mean * mean);

    if (mean < BLACK_THRESHOLD) {
        spdlog::debug("[frame] Detected nearly black frame (mean = {:.1f})", mean);
        return true;
    }

    if (variance < VARIANCE_THRESHOLD) {
        spdlog::debug("[frame] Detected low detail frame (variance = {:.1f})", variance);
        return true;
    }

    return false;
}
*/


} // namespace vpu
