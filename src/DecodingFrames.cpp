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
static constexpr bool KDUMPFRAMES = false;
static constexpr double KSAMPLEPERIOD = 1.0; // seconds → 1 FPS
static constexpr int KMAXHWERRORS = 30;      // non‑fatal errors before abandoning HW
static constexpr uint64_t ALLBLACK = 0x0000000000000000ULL;
static constexpr uint64_t ALLONECOLOUR = 0x8000000000000000ULL;

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
// struct SwsDeleter {
//     void operator()(SwsContext* c) const noexcept { sws_freeContext(c); }
// };

using FmtPtr = std::unique_ptr<AVFormatContext, CDeleter<&avformat_close_input>>;
using CtxPtr = std::unique_ptr<AVCodecContext, CDeleter<&avcodec_free_context>>;
using FrmPtr = std::unique_ptr<AVFrame, CDeleter<&av_frame_free>>;
using PktPtr = std::unique_ptr<AVPacket, CDeleter<&av_packet_free>>;
using BufPtr = std::unique_ptr<AVBufferRef, CDeleter<&av_buffer_unref>>;
// using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

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

using util::sec_to_pts;

static inline bool sample_hit(int64_t pts, int64_t& next_pts, int64_t step)
{
    if (pts == AV_NOPTS_VALUE) {
        next_pts += step;
        return true;
    }
    if (pts >= next_pts) {
        next_pts += step;
        return true;
    }
    return false;
}

// VA‑API  →  dma‑buf (luma)  →  EGLImage  →  GL compute shader
std::vector<uint64_t> decode_and_hash_hw_gl(
    std::string const& file,
    double skip_pct,
    int duration_s,
    int max_frames,
    std::function<void(int)> const& progress)
{
    using namespace util;
    std::vector<uint64_t> hashes;

    auto& tl = gl::ThreadLocalContext::current();
    if (!tl.ensure_current()) {
        spdlog::error("[hw] Failed to ensure GL context");
        return hashes;
    }

    // ── open container ──
    FmtPtr fmt;
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, file.c_str(), nullptr, nullptr) < 0) {
        spdlog::error("[hw] Failed to open input file: {}", file);
        return hashes;
    }
    fmt.reset(raw);

    if (avformat_find_stream_info(raw, nullptr) < 0) {
        spdlog::error("[hw] Failed to find stream info");
        return hashes;
    }

    int vstream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream < 0) {
        spdlog::error("[hw] No video stream found");
        return hashes;
    }
    AVStream* st = fmt->streams[vstream];

    if (duration_s <= 0) {
        if (st->duration != AV_NOPTS_VALUE)
            duration_s = int(st->duration * av_q2d(st->time_base));
        else if (fmt->duration != AV_NOPTS_VALUE)
            duration_s = int(fmt->duration / AV_TIME_BASE);
    }

    // short-clip → no skip (this avoids skipping past the last keyframe and
    // being unable to decode)
    double pct = std::clamp(skip_pct, 0.0, 0.20);
    qint64 file_sz = QFileInfo(QString::fromStdString(file)).size();
    if ((duration_s > 0 && duration_s < 20) || file_sz < 5 * 1024 * 1024)
        pct = 0.0;

    // Compute seek target and initial next_pts
    int64_t skip_pts = pct > 0.0
        ? sec_to_pts(pct * duration_s, st->time_base)
        : 0;
    int64_t next_pts = skip_pts; // declare and initialise local counter
    if (skip_pts > 0) {
        spdlog::info("[hw] Seeking to {:.1f}% ({} seconds)", pct * 100, pct * duration_s);
        if (av_seek_frame(fmt.get(), vstream, skip_pts, AVSEEK_FLAG_BACKWARD) < 0) {
            spdlog::warn("[hw] Seek failed, starting from beginning");
        }
    }

    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        spdlog::error("[hw] No decoder found for codec id: {}", static_cast<int>(st->codecpar->codec_id));
        return hashes;
    }

    // ── hw device ──
    AVBufferRef* hw_dev = nullptr;
    int hw_err = av_hwdevice_ctx_create(&hw_dev, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (hw_err < 0) {
        spdlog::error("[hw] Failed to create hardware device context: {}", ff_err2str(hw_err));
        return hashes;
    }
    util::BufPtr hw_dev_ref(hw_dev);

    CtxPtr codec_ctx { avcodec_alloc_context3(dec) };
    if (!codec_ctx) {
        spdlog::error("[hw] Failed to allocate codec context");
        return hashes;
    }

    if (avcodec_parameters_to_context(codec_ctx.get(), st->codecpar) < 0) {
        spdlog::error("[hw] Failed to copy codec parameters to context");
        return hashes;
    }

    // Create a managed reference for hw_device_ctx
    codec_ctx->hw_device_ctx = av_buffer_ref(hw_dev);
    if (!codec_ctx->hw_device_ctx) {
        spdlog::error("[hw] Failed to reference hardware device context");
        return hashes;
    }

    codec_ctx->get_format = [](AVCodecContext*, AVPixelFormat const* p) {
        for (; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_VAAPI)
                return *p;
        return AV_PIX_FMT_NONE;
    };

    if (avcodec_open2(codec_ctx.get(), dec, nullptr) < 0) {
        spdlog::warn("[hw] avcodec_open2 failed – abandoning HW path early");
        return {}; // let SW take over immediately
    }

    auto* devctx = reinterpret_cast<AVHWDeviceContext*>(hw_dev->data);
    auto* vactx = reinterpret_cast<AVVAAPIDeviceContext*>(devctx->hwctx);
    VADisplay va_dpy = vactx->display;

    // ── GL + dump dir ──
    glpipe::Mean32PipelineGL pipe(tl.api());
    std::array<uint8_t, constants::kOutW * constants::kOutH> gray;
    std::filesystem::path dumpDir;
    if constexpr (KDUMPFRAMES) {
        dumpDir = std::filesystem::path("frames") / QFileInfo(QString::fromStdString(file)).completeBaseName().toStdString();
        std::filesystem::create_directories(dumpDir);
    }

    // ── sampling control ──
    int64_t step_pts = sec_to_pts(KSAMPLEPERIOD, st->time_base);
    if (step_pts <= 0) {
        spdlog::warn("[hw] Invalid step_pts calculated, forcing to 1");
        step_pts = 1;
    }

    FrmPtr frm(av_frame_alloc());
    if (!frm) {
        spdlog::error("[hw] Failed to allocate frame");
        return hashes;
    }

    PktPtr pkt(av_packet_alloc());
    if (!pkt) {
        spdlog::error("[hw] Failed to allocate packet");
        return hashes;
    }

    int lastPct = -1;
    int errorBudget = 0;
    int64_t total_frames = 0; // Track total frames processed for progress reporting

    // Define error classification functions
    auto tooManyErrors = [&]() { return errorBudget >= KMAXHWERRORS; };
    auto bumpError = [&]() {
        if (++errorBudget == KMAXHWERRORS)
            spdlog::warn("[hw] error budget exhausted ({} errors) – falling back to SW", KMAXHWERRORS);
    };

    // Define consistent error handlers
    /*
    auto handleFatalError = [&](int err, char const* context) {
        spdlog::error("[hw] Fatal error in {}: {}", context, ff_err2str(err));
        return true; // signal to exit
    };
    */

    auto handleSoftError = [&](int err, char const* context) {
        spdlog::warn("[hw] Recoverable error in {}: {}", context, ff_err2str(err));
        bumpError();
        return false; // signal to continue
    };

    // Main decoding loop
    while (!tooManyErrors() && (max_frames <= 0 || static_cast<int>(hashes.size()) < max_frames)) {

        int read_result = av_read_frame(fmt.get(), pkt.get());
        if (read_result < 0) {
            if (read_result == AVERROR_EOF) {
                spdlog::info("[hw] End of file reached");
                break; // Normal end of file
            } else {
                if (handleSoftError(read_result, "av_read_frame"))
                    break;
                continue;
            }
        }

        // Skip non-video packets
        if (pkt->stream_index != vstream) {
            av_packet_unref(pkt.get());
            continue;
        }

        int sret = avcodec_send_packet(codec_ctx.get(), pkt.get());
        av_packet_unref(pkt.get());

        if (sret == AVERROR(EAGAIN)) {
            // Buffer full – normal condition, try receiving frames first
            spdlog::debug("[hw] send_packet returned EAGAIN, need to receive frames first");
            // Continue to receive_frame without sending more packets
        } else if (sret == AVERROR(ENOSYS)) {
            // Hardware acceleration not implemented - fatal
            spdlog::error("[hw] Hardware acceleration not implemented ({}) → abort HW", ff_err2str(sret));
            return {}; // immediate SW fallback
        } else if (sret < 0) {
            // All other errors (AVERROR_INVALIDDATA, EINVAL, etc) - increment error budget
            if (handleSoftError(sret, "avcodec_send_packet"))
                break;
            continue;
        }

        // Try to receive frames
        bool need_more_packets = false;
        while (!need_more_packets) {
            int rret = avcodec_receive_frame(codec_ctx.get(), frm.get());

            if (rret == 0) {
                // Successfully received a frame
                total_frames++;

                int64_t cur_pts = frm->best_effort_timestamp;
                bool keep = sample_hit(cur_pts, next_pts, step_pts);

                if (keep) {
                    if (!lumaext::extract_luma_32x32(tl.api(), va_dpy, frm.get(), gray.data())) {
                        spdlog::warn("[hw] extract_luma_32x32 failed ({})", lumaext::to_string(lumaext::last_error()));
                        bumpError();
                    } else {
                        if constexpr (KDUMPFRAMES) {
                            std::ostringstream ss;
                            ss << "hw_" << std::setw(6) << std::setfill('0') << hashes.size() << ".pgm";
                            write_pgm(dumpDir / ss.str(), gray.data(), constants::kOutW, constants::kOutH, constants::kOutW);
                        }

                        if (auto h = compute_phash_from_preprocessed(gray.data())) {

                            if (h != ALLBLACK && h != ALLONECOLOUR) {
                                hashes.push_back(*h);
                            } else {
                                spdlog::info("[hw] Skipping hash that represents an image that is entirely one colour");
                            }

                            // Update progress based on frames or position
                            if (max_frames > 0) {
                                int pct = static_cast<int>(hashes.size() * 100 / max_frames);
                                if (pct != lastPct && progress) {
                                    progress(pct);
                                    lastPct = pct;
                                }
                            } else if (duration_s > 0 && progress) {
                                // Estimate progress based on timestamp when max_frames is not specified
                                if (cur_pts != AV_NOPTS_VALUE) {
                                    double current_sec = cur_pts * av_q2d(st->time_base);
                                    int pct = static_cast<int>(current_sec * 100 / duration_s);
                                    if (pct != lastPct) {
                                        progress(pct);
                                        lastPct = pct;
                                    }
                                }
                            }
                        } else {
                            spdlog::warn("[hw] Failed to compute hash for frame");
                            bumpError();
                        }
                    }
                }

                av_frame_unref(frm.get());
            } else if (rret == AVERROR(EAGAIN)) {
                need_more_packets = true; // Need more packets
            } else if (rret == AVERROR_EOF) {
                spdlog::info("[hw] Decoder signaled EOF, all frames decoded");
                goto end_hw; // Finished cleanly
            } else if (rret == AVERROR(ENOSYS)) {
                // Hardware acceleration not implemented - fatal
                spdlog::error("[hw] receive_frame ENOSYS - hardware acceleration not supported");
                return {}; // Immediate fallback
            } else if (rret == AVERROR_INVALIDDATA || rret == AVERROR(EINVAL)) {
                // For consistency, treat these the same as in send_packet
                if (handleSoftError(rret, "avcodec_receive_frame"))
                    goto end_hw;
                need_more_packets = true;
            } else {
                // Other errors
                if (handleSoftError(rret, "avcodec_receive_frame"))
                    goto end_hw;
                need_more_packets = true;
            }
        }
    }

end_hw:
    if (tooManyErrors()) {
        spdlog::info("[hw] Too many errors, falling back to SW decoding after processing {} frames", total_frames);
        return {}; // exhausted budget – fallback
    }

    spdlog::info("[hw] Successfully processed {} frames, extracted {} hashes", total_frames, hashes.size());
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

    // Ensure we have a GL context for luma extraction
    auto& tl = gl::ThreadLocalContext::current();
    if (!tl.ensure_current()) {
        spdlog::error("[sw] Failed to create/activate GL context");
        return {};
    }
    std::array<uint8_t, constants::kOutW * constants::kOutH> lumaTile;

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
    int64_t const step_pts = util::sec_to_pts(1.0, st->time_base); // 1 s step
    int64_t next_pts = 0;                                          // first target (may be overwritten after seek)

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

    int err = avcodec_open2(codec_ctx.get(), dec, nullptr);
    if (err < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_open2 failed: {}", ff_err2str(err));
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
    hashes.reserve(max_frames > 0 ? max_frames : 128);

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
                    if (lumaext::extract_luma_32x32(tl.api(), frm.get(), lumaTile.data())) {
                        if constexpr (KDUMPFRAMES) {
                            std::ostringstream ss;
                            ss << "sw_" << std::setw(6) << std::setfill('0') << hashes.size() << ".pgm";
                            write_pgm(dumpDir / ss.str(), lumaTile.data(),
                                constants::kOutW, constants::kOutH, constants::kOutW);
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
                        spdlog::warn("[sw] luma extractor failed ({})",
                            lumaext::to_string(lumaext::last_error()));
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
                if (lumaext::extract_luma_32x32(tl.api(), frm.get(), lumaTile.data())) {
                    if constexpr (KDUMPFRAMES) {
                        std::ostringstream ss;
                        ss << "sw_" << std::setw(6) << std::setfill('0') << hashes.size() << ".pgm";
                        write_pgm(dumpDir / ss.str(), lumaTile.data(),
                            constants::kOutW, constants::kOutH, constants::kOutW);
                    }
                    if (auto h = compute_phash_from_preprocessed(lumaTile.data()))
                        hashes.push_back(*h);
                } else {
                    spdlog::warn("[sw] luma extractor failed ({})",
                        lumaext::to_string(lumaext::last_error()));
                }

                next_pts += step_pts; // advance to next second
            } // else: silently skip frame

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
