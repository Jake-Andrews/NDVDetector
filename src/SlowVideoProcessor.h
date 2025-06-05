#pragma once

#include "IVideoProcessor.h"
#include "SearchSettings.h"
#include "VideoInfo.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// RAII wrappers for FFmpeg resources
template<auto Fn>
struct AvDeleter {
    template<class T>
    void operator()(T* p) const noexcept { if (p) Fn(&p); }
};

using FmtPtr = std::unique_ptr<AVFormatContext, AvDeleter<&avformat_close_input>>;
using CtxPtr = std::unique_ptr<AVCodecContext, AvDeleter<&avcodec_free_context>>;
using FrmPtr = std::unique_ptr<AVFrame, AvDeleter<&av_frame_free>>;
using PktPtr = std::unique_ptr<AVPacket, AvDeleter<&av_packet_free>>;

// Thread-safe bounded queue
template<class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t cap) : cap_(cap) {}

    bool push(T&& v, std::stop_token const& tk) {
        std::unique_lock lk(m_);
        cv_not_full_.wait(lk, [&] { return q_.size() < cap_ || tk.stop_requested(); });
        if (tk.stop_requested())
            return false;
        q_.emplace_back(std::move(v));
        cv_not_empty_.notify_one();
        return true;
    }

    bool pop(T& out, std::stop_token const& tk) {
        std::unique_lock lk(m_);
        cv_not_empty_.wait(lk, [&] { return !q_.empty() || tk.stop_requested(); });
        if (tk.stop_requested())
            return false;
        out = std::move(q_.front());
        q_.pop_front();
        cv_not_full_.notify_one();
        return true;
    }

private:
    std::mutex m_;
    std::condition_variable cv_not_empty_, cv_not_full_;
    std::deque<T> q_;
    std::size_t const cap_;
};

// Thread pool for parallel frame hashing
class HashPool {
public:
    HashPool(std::size_t nWorkers, BoundedQueue<FrmPtr>& q,
             std::vector<uint64_t>& outHashes, std::atomic_bool& fatal);
    ~HashPool();

private:
    void worker_loop(std::stop_token tk);

    BoundedQueue<FrmPtr>& q_;
    std::vector<uint64_t>& hashes_;
    std::atomic_bool& fatal_;
    std::vector<std::jthread> workers_;
};

// Main video processing class
class SlowVideoProcessor : public IVideoProcessor {
public:
    std::vector<uint64_t> 
    decodeAndHash(VideoInfo const& info, SearchSettings const& cfg) override;

private:
    void demux_decode_loop(VideoInfo const& info,
                          SearchSettings const& cfg,
                          std::stop_token tk,
                          BoundedQueue<FrmPtr>& frameQ,
                          std::atomic_bool& fatal);
};
