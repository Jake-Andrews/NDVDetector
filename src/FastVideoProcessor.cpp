// FastVideoProcessor.cpp
#include "FastVideoProcessor.h"
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
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "VideoProcessingUtils.h"
using namespace vpu;

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

static constexpr double KSAMPLEPERIODSPS = 0.2; // seconds per sample
static constexpr int KPROBESIZE = 10 * 1024 * 1024;
static constexpr int KANALYZEUSEC = 10 * 1'000'000;

// saves frames to fs for debugging
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
            vpu::report_progress(on_progress, 100);
            return {};
        }
        fmt.reset(raw);
    }
    if (int e = avformat_find_stream_info(fmt.get(), nullptr); e < 0) {
        spdlog::error("[ffmpeg-sw] stream_info: {}", ff_err2str(e));
        vpu::report_progress(on_progress, 100);
        return {};
    }

    int vstream = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream < 0) {
        spdlog::warn("[ffmpeg-sw] no video stream");
        vpu::report_progress(on_progress, 100);
        return {};
    }
    AVStream* st = fmt->streams[vstream];
    int64_t const step_pts = vpu::sec_to_pts(KSAMPLEPERIODSPS, st->time_base);
    int64_t next_pts = 0; // first target (may be overwritten after seek)

    // --- decoder open ---
    AVCodec const* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        spdlog::warn("[ffmpeg-sw] unsupported codec ID {}", static_cast<int>(st->codecpar->codec_id));
        vpu::report_progress(on_progress, 100);
        return {};
    }

    CtxPtr codec_ctx { avcodec_alloc_context3(dec) };
    if (!codec_ctx || avcodec_parameters_to_context(codec_ctx.get(), st->codecpar) < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_parameters_to_context failed");
        vpu::report_progress(on_progress, 100);
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
    codec_ctx->thread_count = 1; // std::max(1u, std::thread::hardware_concurrency());
    spdlog::info("[sw] Using {} threads for SW decoding", codec_ctx->thread_count);
    // codec_ctx->skip_idct = AVDISCARD_ALL;
    codec_ctx->skip_loop_filter = AVDISCARD_ALL;

    int err = avcodec_open2(codec_ctx.get(), dec, nullptr);
    if (err < 0) {
        spdlog::error("[ffmpeg-sw] avcodec_open2 failed: {}", ff_err2str(err));
        report_progress(on_progress, 100);
        return {};
    }

    // --- segment-seek initialisation ---
    // Set frames per segment based on mode (1 or 5 frames)
    bool const oneFrameMode = cfg.fastHash.maxFrames == 2;   // 2 total frames (1 per segment)
    bool const fiveFrameMode = cfg.fastHash.maxFrames == 10; // 10 total frames (5 per segment)
    if (!oneFrameMode && !fiveFrameMode) {
        throw std::runtime_error(std::format(
            "Invalid configuration cfg.fastHash.maxFrames: '{}', must be 2 or 10",
            cfg.fastHash.maxFrames));
    }
    std::size_t const kFramesPerSegment = oneFrameMode ? 1 : 5;
    std::array<double, 2> seekPercents = { 0.30, 0.70 }; // 30%, 70%
    std::size_t currentSegmentIdx = 0;
    std::size_t framesInCurrentSeg = 0;

    auto do_seek = [&](double pct) -> bool {
        if (v.duration <= 0)
            return false;
        int64_t seekPts = sec_to_pts(pct * v.duration, st->time_base);
        if (av_seek_frame(fmt.get(), vstream, seekPts, AVSEEK_FLAG_BACKWARD) < 0) {
            spdlog::warn("[seek] …");
            return false;
        }
        avcodec_flush_buffers(codec_ctx.get());
        // avformat_flush(fmt.get());
        next_pts = seekPts;
        spdlog::info("[seek] jumped to {:.1f}%", pct * 100);
        return true;
    };

    // Initial seek to first segment
    if (!do_seek(seekPercents[currentSegmentIdx])) { // start at 30 %
        spdlog::error("[seek] Initial seek failed");
        report_progress(on_progress, 100);
        return {};
    }

    // --- pre‑allocate frames/packets ---
    FrmPtr frm { av_frame_alloc() };
    PktPtr pkt { av_packet_alloc() };

    std::vector<uint8_t> grayBuf; // scratch for full-res GRAY8 frames

    // --- main decode / hash loop ---
    std::vector<uint64_t> hashes;
    hashes.reserve(cfg.fastHash.maxFrames);

    std::filesystem::path dumpDir;
    if constexpr (KDUMPFRAMES) {
        dumpDir = std::filesystem::path("frames") / QFileInfo(QString::fromStdString(v.path)).completeBaseName().toStdString();
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

        // --- send the packet — retry once on EAGAIN ---
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

            // --- drain frames ---
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

                if (sample_due(frame_pts, next_pts)) {
                    // got a frame → process
                    ++frames_seen;
                    if (auto hval = hash_frame(frm.get(), grayBuf, fatal_error)) {
                        hashes.push_back(*hval);
                        ++framesInCurrentSeg;
                        if (framesInCurrentSeg >= kFramesPerSegment) {
                            framesInCurrentSeg = 0;
                            ++currentSegmentIdx;
                            if (currentSegmentIdx < seekPercents.size()) {
                                if (!do_seek(seekPercents[currentSegmentIdx])) { // go to 70 %
                                    spdlog::error("[seek] Segment seek failed");
                                    fatal_error = true;
                                } else {
                                    break; // successful seek, process next segment
                                }
                            } else {
                                stopped = true; // all segments done
                            }
                            break; // leave inner frame-receive loop after seek/stop
                        }
                        int pct_now = int(hashes.size() * 100 / cfg.fastHash.maxFrames);
                        if (pct_now != last_progress && pct_now <= 100) {
                            last_progress = pct_now;
                            report_progress(on_progress, pct_now);
                        }
                    }
                    next_pts += step_pts; // advance to next second
                }
            } // else: silently skip frame

            av_frame_unref(frm.get());

            if (int(hashes.size()) >= cfg.fastHash.maxFrames) {
                stopped = true;
                break;
            }
            if (sret != AVERROR(EAGAIN))
                break; // packet fully sent or fatal — move to next packet
        }
    }

decode_done:
    // --- flush remaining ---
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

            if (sample_due(frame_pts, next_pts)) {
                ++frames_seen;
                if (auto hval = hash_frame(frm.get(), grayBuf, fatal_error)) {
                    hashes.push_back(*hval);
                    ++framesInCurrentSeg;
                    if (framesInCurrentSeg >= kFramesPerSegment) {
                        framesInCurrentSeg = 0;
                        ++currentSegmentIdx;
                        if (currentSegmentIdx < seekPercents.size()) {
                            if (!do_seek(seekPercents[currentSegmentIdx])) { // go to 70 %
                                spdlog::error("[seek] Segment seek failed");
                                fatal_error = true;
                            } else {
                                break; // successful seek, process next segment
                            }
                        } else {
                            stopped = true; // all segments done
                        }
                        break; // leave inner frame-receive loop after seek/stop
                    }
                }
            }
            next_pts += step_pts; // advance to next second
        } // else: silently skip frame

        av_frame_unref(frm.get());
    }

    spdlog::info("[sw] finished: {} frames seen, {} hashes", frames_seen, hashes.size());
    vpu::report_progress(on_progress, 100);
    return hashes;
}
