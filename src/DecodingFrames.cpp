// DecodingFrames.cpp
#include "DecodingFrames.h"
#include "PHashCPU.h"

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

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// saves frames to fs for debugging
static constexpr double KSAMPLEPERIOD = 1.0; // seconds → 1 FPS
static constexpr uint64_t ALLBLACK = 0x0000000000000000ULL;
static constexpr uint64_t ALLONECOLOUR = 0x8000000000000000ULL;
static constexpr int KOUTH = 32;
static constexpr int KOUTW = 32;
static constexpr int KPROBESIZE = 10 * 1024 * 1024;
static constexpr int KANALYZEUSEC = 10 * 1'000'000;

static constexpr bool KDUMPFRAMES = false;
static void write_pgm(std::filesystem::path const& file,
    uint8_t const* data, int w, int h, int stride)
{
    std::ofstream os(file, std::ios::binary);
    if (!os)
        return;
    os << "P5\n"
       << w << ' ' << h << "\n255\n";
    for (int y = 0; y < h; ++y)
        os.write(reinterpret_cast<char const*>(data + y * stride), w);
}

namespace util {
template<auto Fn>
struct CDeleter {
    template<class T>
    void operator()(T* p) const noexcept
    {
        if (p)
            Fn(&p);
    }
};

using FmtPtr = std::unique_ptr<AVFormatContext, CDeleter<&avformat_close_input>>;
using CtxPtr = std::unique_ptr<AVCodecContext, CDeleter<&avcodec_free_context>>;
using FrmPtr = std::unique_ptr<AVFrame, CDeleter<&av_frame_free>>;
using PktPtr = std::unique_ptr<AVPacket, CDeleter<&av_packet_free>>;
using BufPtr = std::unique_ptr<AVBufferRef, CDeleter<&av_buffer_unref>>;

inline char const* ff_err2str(int e)
{
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(buf, sizeof(buf), e);
    return buf;
}

inline int64_t sec_to_pts(double sec, AVRational tb)
{
    if (tb.num == 0 || tb.den == 0) {
        spdlog::warn("[util] Invalid time base for sec_to_pts: num={}, den={}", tb.num, tb.den);
        return AV_NOPTS_VALUE;
    }
    return static_cast<int64_t>(std::llround(sec / av_q2d(tb)));
}

inline void report_progress(std::function<void(int)> const& cb, int pct)
{
    if (cb)
        cb(pct);
}
} // namespace util

using util::sec_to_pts;

static bool extract_luma_32x32(AVFrame const* src, uint8_t* dst)
{
    static SwsContext* sws = nullptr;
    static int lastW = 0, lastH = 0, lastFmt = AV_PIX_FMT_NONE;

    if (!sws || src->width != lastW || src->height != lastH || src->format != lastFmt) {
        if (sws)
            sws_freeContext(sws);
        sws = sws_getContext(src->width, src->height,
            static_cast<AVPixelFormat>(src->format),
            32, 32, AV_PIX_FMT_GRAY8,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            spdlog::error("[sw] sws_getContext failed");
            return false;
        }
        lastW = src->width;
        lastH = src->height;
        lastFmt = src->format;
    }
    uint8_t* dstData[1] = { dst };
    int dstLines[1] = { 32 };
    return sws_scale(sws, src->data, src->linesize, 0, src->height,
               dstData, dstLines)
        > 0;
}

// CPU/Software decoding path
std::vector<uint64_t> decode_and_hash(
    std::string const& file,
    double skip_pct,
    int duration_s,
    int max_frames,
    std::function<void(int)> const& on_progress)
{
    // ------ early validation / fast failure -----------------
    if (file.empty() || !std::filesystem::exists(file)) {
        spdlog::error("[sw] decode_and_hash: file '{}' not found", file);
        util::report_progress(on_progress, 100);
        return {};
    }
    if (max_frames == 0) // caller asked for nothing
        return {};

    // Reserve a sensible capacity up-front (makes push_back O(1))
    size_t est = (max_frames > 0) ? static_cast<size_t>(max_frames)
                                  : (duration_s > 0 ? static_cast<size_t>(duration_s / KSAMPLEPERIOD) + 1u
                                                    : 128u);

    using namespace util;
    static std::once_flag ffOnce;
    std::call_once(ffOnce, [] { av_log_set_level(AV_LOG_WARNING); });

#ifdef NDEBUG
    av_log_set_level(AV_LOG_ERROR);
#endif

    std::array<uint8_t, KOUTW * KOUTH> lumaTile;

    spdlog::info("[sw] decoding '{}' (skip={}%, duration={} s, limit={})",
        file, skip_pct * 100, duration_s, max_frames);

    /* ───────────────────────────── demuxer open ───────────────────────── */
    FmtPtr fmt;
    {
        AVDictionary* o = nullptr;
        av_dict_set_int(&o, "probesize", KPROBESIZE, 0);
        av_dict_set_int(&o, "analyzeduration", KANALYZEUSEC, 0);
        AVFormatContext* raw = nullptr;
        int e = avformat_open_input(&raw, file.c_str(), nullptr, &o);
        av_dict_free(&o);
        if (e < 0) {
            spdlog::error("[ffmpeg-sw] avformat_open_input: {}", ff_err2str(e));
            util::report_progress(on_progress, 100);
            return {};
        }
        fmt.reset(raw);
    }
    if (int e = avformat_find_stream_info(fmt.get(), nullptr); e < 0) {
        spdlog::error("[ffmpeg-sw] stream_info: {}", ff_err2str(e));
        util::report_progress(on_progress, 100);
        return {};
    }

    int vstream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream < 0) {
        spdlog::warn("[ffmpeg-sw] no video stream");
        util::report_progress(on_progress, 100);
        return {};
    }
    AVStream* st = fmt->streams[vstream];
    int64_t const step_pts = util::sec_to_pts(KSAMPLEPERIOD, st->time_base);
    int64_t next_pts = 0; // first target (may be overwritten after seek)

    /* ─────────────── determine/override duration (caller may give 0) ─── */
    if (st->duration != AV_NOPTS_VALUE) {
        duration_s = static_cast<int>(st->duration * av_q2d(st->time_base));
        spdlog::debug("[sw] Determined duration from stream: {} s", duration_s);
    } else if (fmt->duration != AV_NOPTS_VALUE) {
        duration_s = static_cast<int>(fmt->duration / AV_TIME_BASE);
        spdlog::debug("[sw] Determined duration from container: {} s", duration_s);
    } else {
        duration_s = 0;
        spdlog::warn("[sw] Could not determine video duration.");
    }

    /* ───────────────────────────── decoder open ───────────────────────── */
    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        spdlog::warn("[ffmpeg-sw] unsupported codec ID {}", static_cast<int>(st->codecpar->codec_id));
        util::report_progress(on_progress, 100);
        return {};
    }

    CtxPtr codec_ctx { avcodec_alloc_context3(dec) };
    if (!codec_ctx || avcodec_parameters_to_context(codec_ctx.get(), st->codecpar) < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_parameters_to_context failed");
        util::report_progress(on_progress, 100);
        return {};
    }

    /* multithreading BEFORE avcodec_open2() */
    // files normally cary only one slice per frame, so FF_THREAD_SLICE provides
    // no parallelism
    codec_ctx->thread_type = FF_THREAD_FRAME;
    // Decode only reference frames, skip loop-filter & idct already set above
    codec_ctx->skip_frame = AVDISCARD_NONREF;
    // Let FFmpeg apply codec-specific fast flags
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    // Pick a sane explicit thread count (0 = FFmpeg default = may become 1)
    codec_ctx->thread_count = std::max(1u, std::thread::hardware_concurrency());
    spdlog::info("[sw] Using {} threads for SW decoding", codec_ctx->thread_count);
    codec_ctx->skip_idct = AVDISCARD_ALL;
    codec_ctx->skip_loop_filter = AVDISCARD_ALL;

    int err = avcodec_open2(codec_ctx.get(), dec, nullptr);
    if (err < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_open2 failed: {}", ff_err2str(err));
        util::report_progress(on_progress, 100);
        return {};
    }

    /* ───────────────────── pre‑allocate frames/packets ────────────────── */
    FrmPtr frm { av_frame_alloc() };
    PktPtr pkt { av_packet_alloc() };

    /* ───────────────────────────── seeking logic ─────────────────────── */
    double pct = std::clamp(skip_pct, 0.0, 0.20);
    qint64 file_sz = QFileInfo(QString::fromStdString(file)).size();
    if ((duration_s > 0 && duration_s < 20) || file_sz < 5 * 1024 * 1024) {
        spdlog::info("[sw] small file → skip disabled (was {:.1f}%)", pct * 100);
        pct = 0.0;
    }

    if (duration_s > 0 && pct > 0.0) {
        int64_t seek_pts = sec_to_pts(pct * duration_s, st->time_base);
        if (seek_pts > 0) {
            avformat_flush(fmt.get());
            if (avformat_seek_file(fmt.get(), vstream, INT64_MIN, seek_pts, INT64_MAX, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(codec_ctx.get());
                spdlog::info("[seek] jumped to {:.1f}%", pct * 100);
                next_pts = seek_pts;
            } else {
                spdlog::warn("[seek] avformat_seek_file failed, decoding from start");
            }
        }
    } else {
        spdlog::debug("[sw] Initial skip percentage is 0 or duration unknown, skipping seeking.");
    }

    /* ───────────────────── main decode / hash loop ───────────────────── */
    std::vector<uint64_t> hashes;
    hashes.reserve(est);

    std::filesystem::path dumpDir;
    if constexpr (KDUMPFRAMES) {
        dumpDir = std::filesystem::path("frames") / QFileInfo(QString::fromStdString(file)).completeBaseName().toStdString();
        std::filesystem::create_directories(dumpDir);
    }
    int last_progress = -1;
    size_t frames_seen = 0;
    bool stopped = false;
    bool fatal_error = false;

    while (!fatal_error && !stopped && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != vstream) {
            av_packet_unref(pkt.get());
            continue;
        }

        /* send the packet — retry once on EAGAIN */
        bool packet_retained = true;
        while (packet_retained) {
            int sret = avcodec_send_packet(codec_ctx.get(), pkt.get());
            if (sret == 0) {
                av_packet_unref(pkt.get());
                packet_retained = false; // ownership transferred
            } else if (sret == AVERROR(EAGAIN)) {
                // decoder is full → drain one batch then retry
            } else if (sret == AVERROR_EOF) {
                av_packet_unref(pkt.get());
                packet_retained = false;
                break; // codec already signalled EOF
            } else {
                spdlog::warn("[ffmpeg-sw] send_packet: {}", ff_err2str(sret));
                av_packet_unref(pkt.get());
                fatal_error = true;
                break;
            }

            /* drain frames */
            while (true) {
                int rret = avcodec_receive_frame(codec_ctx.get(), frm.get());
                if (rret == AVERROR(EAGAIN))
                    break; // need more packets
                if (rret == AVERROR_EOF)
                    goto decode_done; // finished
                if (rret < 0) {
                    spdlog::warn("[ffmpeg-sw] receive_frame: {}", ff_err2str(rret));
                    fatal_error = true;
                    break;
                }

                int64_t frame_pts = (frm->pts != AV_NOPTS_VALUE)
                    ? frm->pts
                    : frm->best_effort_timestamp; // fallback

                if (frame_pts == AV_NOPTS_VALUE || frame_pts >= next_pts) {
                    // got a frame → process
                    ++frames_seen;
                    if (extract_luma_32x32(frm.get(), lumaTile.data())) {
                        if constexpr (KDUMPFRAMES) {
                            std::ostringstream ss;
                            ss << "sw_" << std::setw(6) << std::setfill('0') << hashes.size() << ".pgm";
                            write_pgm(dumpDir / ss.str(), lumaTile.data(),
                                KOUTW, KOUTH, KOUTW);
                        }
                        if (auto h = compute_phash_from_preprocessed(lumaTile.data())) {
                            hashes.push_back(*h);
                            if (max_frames > 0) {
                                int pct_now = int(hashes.size() * 100 / max_frames);
                                if (pct_now != last_progress && pct_now <= 100) {
                                    last_progress = pct_now;
                                    report_progress(on_progress, pct_now);
                                }
                            }
                        }
                    } else {
                        spdlog::warn("[sw] luma extractor failed");
                    }

                    next_pts += step_pts; // advance to next second
                } // else: silently skip frame

                av_frame_unref(frm.get());

                if (max_frames > 0 && int(hashes.size()) >= max_frames) {
                    stopped = true;
                    break;
                }
            }
            if (sret != AVERROR(EAGAIN))
                break; // packet fully sent or fatal — move to next packet
        }
    }

decode_done:
    /* ───────────────────────── flush remaining ───────────────────────── */
    if (!fatal_error && !stopped) {
        avcodec_send_packet(codec_ctx.get(), nullptr);
        while (true) {
            int rret = avcodec_receive_frame(codec_ctx.get(), frm.get());
            if (rret == AVERROR_EOF || rret == AVERROR(EAGAIN))
                break;
            if (rret < 0) {
                spdlog::warn("[ffmpeg-sw] flush receive_frame: {}", ff_err2str(rret));
                break;
            }

            int64_t frame_pts = (frm->pts != AV_NOPTS_VALUE)
                ? frm->pts
                : frm->best_effort_timestamp; // fallback

            if (frame_pts == AV_NOPTS_VALUE || frame_pts >= next_pts) {
                ++frames_seen;
                if (extract_luma_32x32(frm.get(), lumaTile.data())) {
                    if constexpr (KDUMPFRAMES) {
                        std::ostringstream ss;
                        ss << "sw_" << std::setw(6) << std::setfill('0') << hashes.size() << ".pgm";
                        write_pgm(dumpDir / ss.str(), lumaTile.data(),
                            KOUTW, KOUTH, KOUTW);
                    }
                    if (auto h = compute_phash_from_preprocessed(lumaTile.data()))
                        if (h != ALLBLACK || h != ALLONECOLOUR) {
                            hashes.push_back(*h);
                        }
                } else {
                    spdlog::warn("[sw] luma extractor failed");
                }

                next_pts += step_pts; // advance to next second
            } // else: silently skip frame

            av_frame_unref(frm.get());
            if (max_frames > 0 && int(hashes.size()) >= max_frames)
                break;
        }
    }

    spdlog::info("[sw] finished: {} frames seen, {} hashes", frames_seen, hashes.size());
    util::report_progress(on_progress, 100);
    return hashes;
}
