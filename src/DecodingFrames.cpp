// DecodingFrames.cpp
#include "DecodingFrames.h"
#include "GLContext.h"
#include "LumaExtractor.h"
#include "Mean32PipelineGL.h"
#include "PHashCPU.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm/drm_fourcc.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <sstream>
#include <string>

#include <QFileInfo>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <iomanip>

// saves frames to fs for debugging
static constexpr bool kDumpFrames = true;

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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace constants {
constexpr int kProbeSize = 10 * 1024 * 1024;
constexpr int kAnalyzeUsec = 10 * 1'000'000;
constexpr int kOutW = 32;
constexpr int kOutH = 32;
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
struct SwsDeleter {
    void operator()(SwsContext* c) const noexcept { sws_freeContext(c); }
};

using FmtPtr = std::unique_ptr<AVFormatContext, CDeleter<&avformat_close_input>>;
using CtxPtr = std::unique_ptr<AVCodecContext, CDeleter<&avcodec_free_context>>;
using FrmPtr = std::unique_ptr<AVFrame, CDeleter<&av_frame_free>>;
using PktPtr = std::unique_ptr<AVPacket, CDeleter<&av_packet_free>>;
using BufPtr = std::unique_ptr<AVBufferRef, CDeleter<&av_buffer_unref>>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

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

inline std::string glErrorString(GLenum err)
{
    switch (err) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_STACK_OVERFLOW:
        return "GL_STACK_OVERFLOW";
    case GL_STACK_UNDERFLOW:
        return "GL_STACK_UNDERFLOW";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";
    default: {
        std::stringstream ss;
        ss << "Unknown GL error 0x" << std::hex << err;
        return ss.str();
    }
    }
}
} // namespace util

namespace gl {
class ThreadLocalContext {
public:
    static ThreadLocalContext& current()
    {
        thread_local ThreadLocalContext ctx;
        return ctx;
    }

    bool ensure_current()
    {
        try {
            if (!m_ctx) {
                spdlog::debug("[GL] Creating new thread-local GLContext");
                m_ctx.emplace();
            } else {
                spdlog::debug("[GL] Making existing thread-local GLContext current");
                m_ctx->makeCurrent();
            }
            return true;
        } catch (std::exception const& e) {
            spdlog::error("[GL] Exception during ensure_current: {}", e.what());
            return false;
        }
    }

    // Provides access to the underlying GLContext API (display, context handles)
    ::GLContext& api()
    {
        if (!m_ctx) {
            // This should not happen if ensure_current() was called and succeeded
            throw std::runtime_error("[GL] Attempted to access GLContext API before ensuring context");
        }
        return *m_ctx;
    }

private:
    ThreadLocalContext() = default;
    ~ThreadLocalContext() = default;
    ThreadLocalContext(ThreadLocalContext const&) = delete;
    ThreadLocalContext& operator=(ThreadLocalContext const&) = delete;

    std::optional<::GLContext> m_ctx;
};
} // namespace gl

// VA‑API  →  dma‑buf (luma)  →  EGLImage  →  GL compute shader
std::vector<uint64_t> decode_and_hash_hw_gl(std::string const& file,
    double skip_pct,
    int duration_s,
    int max_frames,
    std::function<void(int)> const& progress)
{
    using namespace util;
    std::vector<uint64_t> hashes;

    auto& tl = gl::ThreadLocalContext::current();
    if (!tl.ensure_current())
        return hashes;

    // Open container
    FmtPtr fmt;
    AVFormatContext* fmtPtr = nullptr;
    if (avformat_open_input(&fmtPtr, file.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(fmtPtr, nullptr) < 0)
        return hashes;
    fmt.reset(fmtPtr);

    int vstream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream < 0)
        return hashes;
    AVStream* st = fmt->streams[vstream];
    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec)
        return hashes;

    // **temporary**
    // Caller should be setting the duration and filtering duration <= 0
    if (duration_s <= 0) {
        if (st->duration != AV_NOPTS_VALUE)
            duration_s = int(st->duration * av_q2d(st->time_base));
        else if (fmt->duration != AV_NOPTS_VALUE)
            duration_s = int(fmt->duration / AV_TIME_BASE);
    }

    // Allocate and configure AVCodecContext, used for decoding
    // AVHWDeviceContext -> hw_AV_HWDEVICE_TYPE_VAAPI for hardware accelerated
    // decoding
    // AVPixelFormat -> AV_PIX_FMT_VAAPI for zero copy
    AVBufferRef* hw_dev = nullptr;
    if (av_hwdevice_ctx_create(&hw_dev, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0)
        return hashes;
    util::BufPtr hw_dev_ref(hw_dev);

    CtxPtr codec_ctx { avcodec_alloc_context3(dec) };
    if (avcodec_parameters_to_context(codec_ctx.get(), st->codecpar) < 0)
        return {};

    codec_ctx->hw_device_ctx = av_buffer_ref(hw_dev);
    codec_ctx->get_format = [](AVCodecContext*, AVPixelFormat const* l) -> AVPixelFormat {
        while (*l != AV_PIX_FMT_NONE) {
            if (*l == AV_PIX_FMT_VAAPI)
                return AV_PIX_FMT_VAAPI;
            ++l;
        }
        return AV_PIX_FMT_NONE;
    };

    /* decode only key-frames */
    codec_ctx->skip_frame = AVDISCARD_NONKEY; // discard every non-key frame

    if (avcodec_open2(codec_ctx.get(), dec, nullptr) < 0) {
        spdlog::error("[ffmpeg-hw] failed to open HW decoder");
        return hashes;
    }

    auto* devctx = reinterpret_cast<AVHWDeviceContext*>(hw_dev->data);
    auto* vactx = reinterpret_cast<AVVAAPIDeviceContext*>(devctx->hwctx);
    VADisplay va_dpy = vactx->display;

    // GL compute shader
    glpipe::Mean32PipelineGL pipe(tl.api());
    std::array<uint8_t, constants::kOutW * constants::kOutH> gray;

    std::filesystem::path dumpDir;
    if constexpr (kDumpFrames) {
        dumpDir = std::filesystem::path("frames") / QFileInfo(QString::fromStdString(file)).completeBaseName().toStdString();
        std::filesystem::create_directories(dumpDir);
    }

    // Seek
    double pct = std::clamp(skip_pct, 0.0, 0.2);
    int64_t seek_pts = AV_NOPTS_VALUE;
    if (duration_s > 0 && pct > 0.0) {
        seek_pts = sec_to_pts(pct * duration_s, st->time_base);
        if (seek_pts > 0) {
            av_seek_frame(fmt.get(), vstream, seek_pts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codec_ctx.get());
        }
    }

    // Main decode loop
    FrmPtr frm(av_frame_alloc());
    PktPtr pkt(av_packet_alloc());
    int lastPct = -1;

    while ((max_frames <= 0 || (int)hashes.size() < max_frames) && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != vstream || !(pkt->flags & AV_PKT_FLAG_KEY)) {
            av_packet_unref(pkt.get()); // skip non-video or non-key packets
            continue;
        }
        avcodec_send_packet(codec_ctx.get(), pkt.get());
        av_packet_unref(pkt.get());
        while (avcodec_receive_frame(codec_ctx.get(), frm.get()) == 0) {

            GLContext& gl_ctx = tl.api();

            if (lumaext::extract_luma_32x32(gl_ctx, va_dpy,
                    frm.get(), gray.data())) {
                if constexpr (kDumpFrames) {
                    std::ostringstream ss;
                    ss << "hw_" << std::setw(6) << std::setfill('0') << hashes.size() << ".pgm";
                    write_pgm(dumpDir / ss.str(), gray.data(),
                        constants::kOutW, constants::kOutH,
                        constants::kOutW); // packed 32 × 32
                }
                if (auto h = compute_phash_from_preprocessed(gray.data()))
                    hashes.push_back(*h);
            }

            av_frame_unref(frm.get());
            if (max_frames > 0 && (int)hashes.size() >= max_frames)
                break;
            if (max_frames > 0) {
                int pct = int(hashes.size() * 100 / max_frames);
                if (pct != lastPct) {
                    progress(pct);
                    lastPct = pct;
                }
            }
        }
    }
    return hashes;
}

// CPU/Software decoding path
std::vector<uint64_t> decode_and_hash_sw(
    std::string const& file,
    double skip_pct,
    int duration_s,
    int max_frames,
    std::function<void(int)> const& on_progress)
{
    using namespace util;

    spdlog::info("[sw] decoding '{}' (skip={}%, duration={} s, limit={})",
        file, skip_pct * 100, duration_s, max_frames);

    /* ───────────────────────────── demuxer open ───────────────────────── */
    FmtPtr fmt;
    {
        AVDictionary* o = nullptr;
        av_dict_set_int(&o, "probesize", constants::kProbeSize, 0);
        av_dict_set_int(&o, "analyzeduration", constants::kAnalyzeUsec, 0);
        AVFormatContext* raw = nullptr;
        int e = avformat_open_input(&raw, file.c_str(), nullptr, &o);
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
        return {};
    }

    CtxPtr codec_ctx { avcodec_alloc_context3(dec) };
    if (!codec_ctx || avcodec_parameters_to_context(codec_ctx.get(), st->codecpar) < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_parameters_to_context failed");
        return {};
    }

    /* multithreading BEFORE avcodec_open2() */
    codec_ctx->thread_type = FF_THREAD_SLICE;
    codec_ctx->thread_count = std::max(1u, std::thread::hardware_concurrency());
    spdlog::info("[sw] Using {} threads for SW decoding", codec_ctx->thread_count);

    /* decode only key-frames */
    codec_ctx->skip_frame = AVDISCARD_NONKEY; // discard every non-key frame

    int err = avcodec_open2(codec_ctx.get(), dec, nullptr);
    if (err < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_open2 failed: {}", ff_err2str(err));
        return {};
    }

    /* ───────────────────── pre‑allocate frames/packets ────────────────── */
    FrmPtr frm { av_frame_alloc() };
    PktPtr pkt { av_packet_alloc() };
    SwsPtr swsCtx;

    // Phash requires grayscale, need to convert if not already gray8
    if (codec_ctx->pix_fmt != AV_PIX_FMT_GRAY8) {
        // SWS_POINT is used by the official PHash algorithm
        swsCtx.reset(sws_getContext(codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
            codec_ctx->width, codec_ctx->height, AV_PIX_FMT_GRAY8,
            SWS_POINT, nullptr, nullptr, nullptr));
        if (!swsCtx) {
            spdlog::error("[sw] Failed to create SwsContext → abort");
            return {};
        }
    } else {
        spdlog::debug("[sw] Input format is already GRAY8, no scaling needed.");
    }

    FrmPtr gray { av_frame_alloc() };
    gray->format = AV_PIX_FMT_GRAY8;
    gray->width = codec_ctx->width;
    gray->height = codec_ctx->height;
    if (av_frame_get_buffer(gray.get(), 32) < 0) {
        spdlog::error("[sw] Failed to alloc gray buffer");
        return {};
    }

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
            } else {
                spdlog::warn("[seek] avformat_seek_file failed, decoding from start");
            }
        }
    } else {
        spdlog::debug("[sw] Initial skip percentage is 0 or duration unknown, skipping seeking.");
    }

    /* ───────────────────── main decode / hash loop ───────────────────── */
    std::vector<uint64_t> hashes;
    hashes.reserve(max_frames > 0 ? max_frames : 128);

    std::filesystem::path dumpDir;
    if constexpr (kDumpFrames) {
        dumpDir = std::filesystem::path("frames") / QFileInfo(QString::fromStdString(file)).completeBaseName().toStdString();
        std::filesystem::create_directories(dumpDir);
    }
    int last_progress = -1;
    size_t frames_seen = 0;
    bool stopped = false;
    bool fatal_error = false;

    while (!fatal_error && !stopped && av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != vstream || !(pkt->flags & AV_PKT_FLAG_KEY)) {
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

                // got a frame → process
                ++frames_seen;
                AVFrame* src = frm.get();
                if (swsCtx) {
                    int h = sws_scale(swsCtx.get(), frm->data, frm->linesize, 0, frm->height,
                        gray->data, gray->linesize);
                    if (h != frm->height)
                        spdlog::warn("[sw] sws_scale short: {}/{}", h, frm->height);
                    src = gray.get();
                }

                if constexpr (kDumpFrames) {
                    // we dump the *original* (pre-mean / pre-resize) luma plane
                    uint8_t const* dumpPlane = (codec_ctx->pix_fmt == AV_PIX_FMT_GRAY8)
                        ? src->data[0]
                        : gray->data[0];
                    int dumpStride = (codec_ctx->pix_fmt == AV_PIX_FMT_GRAY8)
                        ? src->linesize[0]
                        : gray->linesize[0];
                    std::ostringstream ss;
                    ss << "sw_" << std::setw(6) << std::setfill('0') << hashes.size() << ".pgm";
                    write_pgm(dumpDir / ss.str(), dumpPlane, src->width, src->height, dumpStride);
                }

                if (auto h = compute_phash_full(src->data[0], src->width, src->height)) {
                    hashes.push_back(*h);
                    if (max_frames > 0) {
                        int pct_now = int(hashes.size() * 100 / max_frames);
                        if (pct_now != last_progress && pct_now <= 100) {
                            last_progress = pct_now;
                            report_progress(on_progress, pct_now);
                        }
                    }
                }

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
            ++frames_seen;
            AVFrame* src = frm.get();
            if (swsCtx)
                sws_scale(swsCtx.get(), frm->data, frm->linesize, 0, frm->height, gray->data, gray->linesize), src = gray.get();
            if (auto h = compute_phash_full(src->data[0], src->width, src->height))
                hashes.push_back(*h);
            av_frame_unref(frm.get());
            if (max_frames > 0 && int(hashes.size()) >= max_frames)
                break;
        }
    }

    spdlog::info("[sw] finished: {} frames seen, {} hashes", frames_seen, hashes.size());
    return hashes;
}

// Public entry point
std::vector<uint64_t> extract_phashes_from_video(
    std::string const& file,
    double skip_pct,
    int duration_s,
    int max_frames,
    bool allow_hw,
    std::function<void(int)> const& on_progress)
{
    static std::once_flag ffmpeg_log_flag;
    std::call_once(ffmpeg_log_flag, [] {
        av_log_set_level(AV_LOG_WARNING);
        spdlog::info("Set FFmpeg internal log level to WARNING.");
    });

    std::vector<uint64_t> hashes_result;
    bool hw_attempted = false;

    allow_hw = true; // **fix, caller should set this**

    if (allow_hw) {
        spdlog::info("Hardware acceleration allowed, attempting HW path...");
        hw_attempted = true;
        try {
            hashes_result = decode_and_hash_hw_gl(file, skip_pct, duration_s, max_frames, on_progress);
        } catch (std::exception const& e) {
            spdlog::error("[extract] Exception caught during HW decode attempt: {}", e.what());
            hashes_result.clear(); // Ensure fallback happens
        } catch (...) {
            spdlog::error("[extract] Unknown exception caught during HW decode attempt.");
            hashes_result.clear(); // Ensure fallback happens
        }
    } else {
        spdlog::info("Hardware acceleration disabled by caller.");
    }

    // Fallback to SW if HW was disabled, not attempted, failed, or produced no hashes
    if (hashes_result.empty()) {
        if (hw_attempted) {
            spdlog::warn("[extract] HW path failed or produced no hashes → SW fallback initiated.");
        } else if (!allow_hw) {
            spdlog::info("[extract] HW path disabled → SW path initiated.");
        } else {
            // Should not happen if allow_hw was true, implies HW path wasn't even entered
            spdlog::warn("[extract] HW path not attempted/skipped? → SW fallback initiated.");
        }
        try {
            hashes_result = decode_and_hash_sw(file, skip_pct, duration_s, max_frames, on_progress);
        } catch (std::exception const& e) {
            spdlog::error("[extract] Exception caught during SW decode attempt: {}", e.what());
            hashes_result.clear();
        } catch (...) {
            spdlog::error("[extract] Unknown exception caught during SW decode attempt.");
            hashes_result.clear();
        }
    } else {
        spdlog::info("[extract] HW path succeeded.");
    }

    spdlog::info("[extract] Finished pHash extraction for '{}'. Found {} hashes.", file, hashes_result.size());
    return hashes_result;
}
