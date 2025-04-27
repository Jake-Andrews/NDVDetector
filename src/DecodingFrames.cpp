#include "PHashCalculator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

// ─── libplacebo ───────────────────────────────────────────────
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>

// ─── FFmpeg ───────────────────────────────────────────────────
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

namespace {

// -------------------------------------------------------------
//  Helper utilities
// -------------------------------------------------------------
inline char const* ff_err2str(int err) noexcept
{
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    return av_make_error_string(buf, sizeof(buf), err);
}

inline std::string_view pix_fmt_name(AVPixelFormat fmt) noexcept
{
    if (auto* n = av_get_pix_fmt_name(fmt))
        return n;
    return "unknown";
}

template<auto Fn>
struct CDeleter {
    template<typename T>
    void operator()(T* p) const noexcept
    {
        if (p)
            Fn(&p);
    }
};

struct SwsDeleter {
    void operator()(SwsContext* ctx) const noexcept { sws_freeContext(ctx); }
};

using LogPtr = std::unique_ptr<pl_log_t const, CDeleter<&pl_log_destroy>>;
using DispPtr = std::unique_ptr<pl_dispatch, CDeleter<&pl_dispatch_destroy>>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using FmtPtr = std::unique_ptr<AVFormatContext, CDeleter<&avformat_close_input>>;
using CtxPtr = std::unique_ptr<AVCodecContext, CDeleter<&avcodec_free_context>>;
using FrmPtr = std::unique_ptr<AVFrame, CDeleter<&av_frame_free>>;
using PktPtr = std::unique_ptr<AVPacket, CDeleter<&av_packet_free>>;
using BufPtr = std::unique_ptr<AVBufferRef, CDeleter<&av_buffer_unref>>;

// ── Vulkan RAII wrapper ───────────────────────────────────────
struct VulkanHandle {
    pl_vulkan vk { nullptr };
    VulkanHandle() = default;
    explicit VulkanHandle(pl_vulkan v)
        : vk(v)
    {
    }
    VulkanHandle(VulkanHandle const&) = delete;
    VulkanHandle& operator=(VulkanHandle const&) = delete;
    VulkanHandle(VulkanHandle&& o) noexcept
        : vk(std::exchange(o.vk, nullptr))
    {
    }
    VulkanHandle& operator=(VulkanHandle&& o) noexcept
    {
        if (this != &o) {
            reset();
            vk = std::exchange(o.vk, nullptr);
        }
        return *this;
    }
    ~VulkanHandle() { reset(); }
    void reset(pl_vulkan new_vk = nullptr)
    {
        if (vk)
            pl_vulkan_destroy(&vk);
        vk = new_vk;
    }
    [[nodiscard]] pl_vulkan get() const noexcept { return vk; }
    [[nodiscard]] explicit operator bool() const noexcept { return vk != nullptr; }
};

// ── Constants ────────────────────────────────────────────────
static constexpr int kProbeSize = 10 * 1024 * 1024; // 10 MiB
static constexpr int kAnalyzeUsec = 10 * 1'000'000; // 10 s
static constexpr int kOutW = 32, kOutH = 32;
static constexpr AVPixelFormat kOutPix = AV_PIX_FMT_GRAY8;

// ── Logging helpers ───────────────────────────────────────────
inline void log_backend_choice(bool hw_ok,
    AVHWDeviceType dev,
    AVPixelFormat hwfmt,
    bool gpu_ok)
{
    if (hw_ok) {
        spdlog::info("[decode] Using HW decode: {} (fmt {})",
            av_hwdevice_get_type_name(dev),
            pix_fmt_name(hwfmt));
        if (gpu_ok)
            spdlog::info("[gpu]   GPU → CPU zero-copy path active");
        else
            spdlog::info("[gpu]   GPU interop disabled (no Vulkan or mapping failed)");
    } else {
        spdlog::info("[decode] Using *software* decode path");
    }
}

// ── Misc helpers ──────────────────────────────────────────────
inline int64_t sec_to_pts(double sec, AVRational tb)
{
    return static_cast<int64_t>(std::llround(sec / av_q2d(tb)));
}

// ── Optional Vulkan backend ───────────────────────────────────
VulkanHandle make_vk(LogPtr const& log)
{
    pl_vulkan_params params {
        .async_transfer = true,
        .async_compute = true,
        .queue_count = 1,
    };
    if (auto* vk = pl_vulkan_create(log.get(), &params)) {
        spdlog::info("[gpu] Vulkan backend initialised");
        return VulkanHandle { vk };
    }
    spdlog::warn("[gpu] Vulkan backend initialisation failed – continuing without GPU optimisation");
    return {};
}

} // namespace

// ============================================================================
//  PUBLIC API
// ============================================================================
std::vector<uint64_t> extract_phashes_from_video(std::string const& file,
    double skip_pct,
    int duration_s,
    AVHWDeviceType hw_backend,
    int max_frames)
{
    spdlog::debug("[decode] Starting pHash extraction for '{}'", file);

    // ── libplacebo logging (mirrors to spdlog at INFO) ────────
    pl_log_params lp {
        .log_cb = pl_log_simple,
        .log_level = PL_LOG_INFO
    };
    LogPtr log { pl_log_create(PL_API_VER, &lp) };

    // ── Optional GPU backend (only if caller requested Vulkan) ─
    VulkanHandle vk;
    if (hw_backend == AV_HWDEVICE_TYPE_VULKAN)
        vk = make_vk(log);

    pl_gpu gpu = vk ? vk.get()->gpu : nullptr;
    bool gpu_ok = gpu != nullptr;

    // ── Open input container ──────────────────────────────────
    FmtPtr fmt;
    {
        AVDictionary* fmt_opts = nullptr;
        av_dict_set_int(&fmt_opts, "probesize", kProbeSize, 0);
        av_dict_set_int(&fmt_opts, "analyzeduration", kAnalyzeUsec, 0);

        AVFormatContext* raw = nullptr;
        if (int err = avformat_open_input(&raw, file.c_str(), nullptr, &fmt_opts); err < 0) {
            spdlog::error("[decode] avformat_open_input: {}", ff_err2str(err));
            av_dict_free(&fmt_opts);
            return {};
        }
        av_dict_free(&fmt_opts);
        fmt.reset(raw);
    }

    if (int err = avformat_find_stream_info(fmt.get(), nullptr); err < 0) {
        spdlog::error("[decode] avformat_find_stream_info: {}", ff_err2str(err));
        return {};
    }

    int vstream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream < 0) {
        spdlog::warn("[decode] No video stream found");
        return {};
    }
    AVStream* st = fmt->streams[vstream];
    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        spdlog::warn("[decode] No decoder for codec {}", int(st->codecpar->codec_id));
        return {};
    }

    // ── Codec context creation helpers ────────────────────────
    auto make_ctx = [&](bool hw) -> CtxPtr {
        CtxPtr ctx { avcodec_alloc_context3(dec) };
        if (!ctx)
            return {};
        if (avcodec_parameters_to_context(ctx.get(), st->codecpar) < 0)
            return {};
        ctx->thread_type = FF_THREAD_FRAME;
        ctx->thread_count = hw ? 0u
                               : std::max(1u, std::thread::hardware_concurrency());
        return ctx;
    };

    CtxPtr cc;                              // final codec context
    BufPtr hwdev;                           // retained hw device
    AVPixelFormat hw_fmt = AV_PIX_FMT_NONE; // chosen hw pixfmt

    // ── Attempt HW decode initialisation ─────────────────────
    if (hw_backend != AV_HWDEVICE_TYPE_NONE) {
        for (int i = 0; auto* cfg = avcodec_get_hw_config(dec, i); ++i) {
            if (cfg->device_type == hw_backend && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                hw_fmt = cfg->pix_fmt;
                break;
            }
        }

        if (hw_fmt != AV_PIX_FMT_NONE) {
            AVBufferRef* tmp = nullptr;
            if (av_hwdevice_ctx_create(&tmp, hw_backend, nullptr, nullptr, 0) == 0) {
                hwdev.reset(tmp);
                cc = make_ctx(true);
                if (cc) {
                    cc->hw_device_ctx = av_buffer_ref(hwdev.get());
                    cc->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(hw_fmt));
                    cc->get_format = [](AVCodecContext* c, AVPixelFormat const* fmts) {
                        auto want = static_cast<AVPixelFormat>(
                            reinterpret_cast<intptr_t>(c->opaque));
                        for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
                            if (*p == want)
                                return *p;
                        return fmts[0];
                    };
                    if (avcodec_open2(cc.get(), dec, nullptr) < 0) {
                        spdlog::warn("[decode] HW initialisation failed – will fall back");
                        cc.reset();
                    }
                }
            } else {
                spdlog::warn("[decode] av_hwdevice_ctx_create failed – will fall back");
            }
        } else {
            spdlog::info("[decode] No matching HW pixfmt for backend {}", av_hwdevice_get_type_name(hw_backend));
        }
    }

    // ── Fallback to software decode if needed ─────────────────
    bool using_hw = static_cast<bool>(cc);
    if (!cc) {
        cc = make_ctx(false);
        if (!cc || avcodec_open2(cc.get(), dec, nullptr) < 0) {
            spdlog::error("[decode] Unable to open decoder in either HW or SW mode");
            return {};
        }
        gpu_ok = false; // interop makes no sense on SW path
    }

    log_backend_choice(using_hw, hw_backend, hw_fmt, gpu_ok);

    // ── Software scaler (CPU) ─────────────────────────────────
    SwsPtr sws_ctx;
    int srcW = 0, srcH = 0;
    AVPixelFormat srcFmt = AV_PIX_FMT_NONE;

    auto ensure_sws = [&](int w, int h, AVPixelFormat fmt) {
        if (sws_ctx && w == srcW && h == srcH && fmt == srcFmt)
            return true;
        sws_ctx.reset(sws_getContext(w, h, fmt,
            kOutW, kOutH, kOutPix,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
        srcW = w;
        srcH = h;
        srcFmt = fmt;
        if (!sws_ctx)
            spdlog::error("[sws] Failed to allocate context ({}×{}, {})",
                w, h, pix_fmt_name(fmt));
        return static_cast<bool>(sws_ctx);
    };

    // ── Frame / packet buffers ────────────────────────────────
    FrmPtr frm { av_frame_alloc() };
    FrmPtr swf { av_frame_alloc() };
    PktPtr pkt { av_packet_alloc() };

    std::array<uint8_t, kOutW * kOutH> gray_buf {};
    FrmPtr out { av_frame_alloc() };
    av_image_fill_arrays(out->data, out->linesize,
        gray_buf.data(), kOutPix, kOutW, kOutH, 1);
    out->width = kOutW;
    out->height = kOutH;
    out->format = kOutPix;

    // ── Optional GPU texture ──────────────────────────────────
    pl_tex dst_tex = nullptr;
    auto create_dst_tex = [&]() -> pl_tex {
        pl_tex_params tp {};
        tp.w = kOutW;
        tp.h = kOutH;
        tp.sampleable = true;
        tp.blit_dst = true;
        tp.host_readable = true;
        tp.format = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 8, 0,
            static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE | PL_FMT_CAP_BLITTABLE));
        return pl_tex_create(gpu, &tp);
    };

    // ── Seek / time-range calculation ─────────────────────────
    double pct = std::clamp(skip_pct, 0.0, 0.40);
    int64_t next_pts = sec_to_pts(pct * duration_s, st->time_base);
    int64_t end_pts = sec_to_pts((1.0 - pct) * duration_s, st->time_base);
    int64_t step_pts = sec_to_pts(1.0, st->time_base);

    if (next_pts && av_seek_frame(fmt.get(), vstream, next_pts, AVSEEK_FLAG_BACKWARD) >= 0) {
        spdlog::debug("[decode] Seeked to {}", next_pts);
        avcodec_flush_buffers(cc.get());
    }

    // ── Lambda helpers ────────────────────────────────────────
    std::vector<uint64_t> hashes;
    hashes.reserve(max_frames > 0 ? max_frames : 64);

    auto push_hash = [&](uint8_t const* g) {
        if (auto h = PHashCalculator::calculatePHash(g, kOutW, kOutH))
            hashes.push_back(*h);
    };

    auto cpu_process = [&](AVFrame* f) {
        if (!ensure_sws(f->width, f->height, static_cast<AVPixelFormat>(f->format)))
            return;
        sws_scale(sws_ctx.get(), f->data, f->linesize, 0, f->height,
            out->data, out->linesize);
        push_hash(out->data[0]);
    };

    auto gpu_process = [&](AVFrame* f) {
        if (!gpu_ok)
            return;

        pl_frame pf {};
        pl_avframe_params params { .frame = f, .tex = nullptr };
        if (!pl_map_avframe_ex(gpu, &pf, &params)) {
            spdlog::warn("[gpu] pl_map_avframe_ex failed – disabling GPU optimisation");
            gpu_ok = false;
            return;
        }

        if (!dst_tex && !(dst_tex = create_dst_tex())) {
            spdlog::warn("[gpu] Could not create destination texture – disabling GPU optimisation");
            gpu_ok = false;
            pl_unmap_avframe(gpu, &pf);
            return;
        }

        pl_tex_blit_params blit {
            .src = pf.planes[0].texture,
            .dst = dst_tex,
            .sample_mode = PL_TEX_SAMPLE_LINEAR
        };
        pl_tex_blit(gpu, &blit);
        pl_unmap_avframe(gpu, &pf);

        pl_tex_transfer_params tp {
            .tex = dst_tex,
            .row_pitch = static_cast<size_t>(kOutW),
            .timer = nullptr,
            .ptr = gray_buf.data()
        };
        if (pl_tex_download(gpu, &tp))
            push_hash(gray_buf.data());
        else
            spdlog::warn("[gpu] Texture download failed");
    };

    // ── Main decode loop ──────────────────────────────────────
    while (true) {
        if (av_read_frame(fmt.get(), pkt.get()) < 0) {
            // Fabricate EOF
            pkt->data = nullptr;
            pkt->size = 0;
        }

        if (pkt->data && pkt->stream_index != vstream) {
            av_packet_unref(pkt.get());
            continue;
        }

        int err = avcodec_send_packet(cc.get(), pkt.get());
        if (err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_EOF)
            spdlog::warn("[decode] avcodec_send_packet: {}", ff_err2str(err));
        av_packet_unref(pkt.get());

        while (avcodec_receive_frame(cc.get(), frm.get()) == 0) {
            int64_t pts = frm->best_effort_timestamp;
            if (pts < next_pts || pts > end_pts) {
                av_frame_unref(frm.get());
                continue;
            }
            next_pts += step_pts;

            bool frame_is_hw = (frm->format == hw_fmt);
            if (frame_is_hw && gpu_ok) {
                gpu_process(frm.get());
            } else if (frame_is_hw && !gpu_ok) {
                if (av_hwframe_transfer_data(swf.get(), frm.get(), 0) == 0)
                    cpu_process(swf.get());
            } else {
                cpu_process(frm.get());
            }
            av_frame_unref(frm.get());

            if (max_frames > 0 && static_cast<int>(hashes.size()) >= max_frames)
                goto finished;
        }

        if (!pkt->data)
            break; // EOF
    }

finished:
    if (gpu && dst_tex)
        pl_tex_destroy(gpu, &dst_tex);

    spdlog::info("[decode] Extracted {} hashes from '{}'", hashes.size(), file);
    return hashes;
}
