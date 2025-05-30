#include "Hash.h"

#include <immintrin.h> // SSE2/AVX2 intrinsics (SIMD down-scale)
#include <iostream>
#include <optional>
#include <vector>

static CImg<float> ph_dct_matrix(int const N)
{
    CImg<float> matrix(N, N, 1, 1, 1 / sqrt((float)N));
    float const c1 = sqrt(2.0 / N);
    for (int x = 0; x < N; x++) {
        for (int y = 1; y < N; y++) {
            matrix(x, y) = c1 * cos((cimg::PI / 2 / N) * y * (2 * x + 1));
        }
    }
    return matrix;
}

static CImg<float> const dct_matrix = ph_dct_matrix(32);
static CImg<float> const kMean7(7, 7, 1, 1, 1.f);

int ph_dct_imagehash_from_buffer(CImg<float> const& img, ulong& hash)
{
    CImg<float> const& C = dct_matrix;
    CImg<float> Ctransp = C.get_transpose();

    CImg<float> dctImage = (C)*img * Ctransp;

    CImg<float> subsec = dctImage.crop(1, 1, 8, 8).unroll('x');

    float median = subsec.median();
    hash = 0;
    for (int i = 0; i < 64; i++, hash <<= 1) {
        float current = subsec(i);
        if (current > median)
            hash |= 0x01;
    }

    return 0;
}

std::vector<uint64_t> generate_pHashes(std::vector<CImg<float>> const& images)
{
    std::vector<uint64_t> results;

    for (auto const& img : images) {

        constexpr uint64_t solidBlackHash = 0x0000000000000000ULL;
        constexpr uint64_t solidColorHash = 0x8000000000000000ULL;

        uint64_t hashVal = 0;
        if ((ph_dct_imagehash_from_buffer(img, hashVal)) < 0) {
            std::cerr << "[Warning] Unable to compute pHash.\n";
            continue;
        }

        if (hashVal == solidBlackHash || hashVal == solidColorHash) {
            std::cout << "[INFO] pHash represents a screenshot that is entirely one colour, therefore skipping.\n";
            continue;
        }

        results.push_back(std::move(hashVal));
    }

    return results;
}

void print_pHashes(std::vector<Hash> const& results)
{
    std::cout << "Computed " << results.size() << " pHash values:\n";
    for (auto const& r : results) {
        std::cout << " => Hash: " << r.value << "\n";
    }
}

std::optional<uint64_t>
compute_phash_from_preprocessed(uint8_t const* gray)
{
    if (!gray)
        return std::nullopt;

    // Load 32×32 gray buffer into CImg<float>
    CImg<float> img(gray, 32, 32, 1, 1);
    img.convolve(kMean7);

    uint64_t h = 0;
    if (ph_dct_imagehash_from_buffer(img, h) < 0)
        return std::nullopt;

    return h;
}

// ---------------------------------------------------------------------
// Fast 32×32 box-filter down-scale with SIMD.
//  – Pre-computes index/weight tables once per (srcW,srcH) combination.
//  – Uses separable horizontal+vertical pass, inner loops vectorised.
// ---------------------------------------------------------------------
namespace simd_ds {

struct Weights { // pre-computed tables
    std::vector<int> idx;
    std::vector<float> w0, w1; // 2-tap box filter
};

static thread_local int cacheW = 0, cacheH = 0;
static thread_local Weights wx, wy;

static void build_table(int src, int dst, Weights& tab)
{
    tab.idx.resize(dst);
    tab.w0.resize(dst);
    tab.w1.resize(dst);

    double scale = static_cast<double>(src) / dst;
    for (int d = 0; d < dst; ++d) {
        double s = (d + 0.5) * scale - 0.5;
        int i0 = static_cast<int>(std::floor(s));
        double f = s - i0;
        int i1 = std::min(i0 + 1, src - 1);

        i0 = std::clamp(i0, 0, src - 1);
        tab.idx[d] = i0;
        tab.w1[d] = static_cast<float>(f);
        tab.w0[d] = 1.0f - tab.w1[d];
    }
}

// Horizontal pass: src (float)  → tmp (float)  (dstW = 32)
static void hpass(float const* src, int srcW, int srcH, int srcStride,
    float* tmp, Weights const& wx)
{
    constexpr int W = 32;
#if defined(__AVX2__)
    for (int y = 0; y < srcH; ++y) {
        float const* row = src + y * srcStride;
        float* out = tmp + y * W;

        int d = 0;
        for (; d + 7 < W; d += 8) {
            __m256 p0 = _mm256_i32gather_ps(row, _mm256_loadu_si256(reinterpret_cast<__m256i const*>(&wx.idx[d])), 4);
            __m256 p1 = _mm256_i32gather_ps(row, _mm256_loadu_si256(reinterpret_cast<__m256i const*>(&wx.idx[d])) + _mm256_set1_epi32(1), 4);
            __m256 w0 = _mm256_loadu_ps(&wx.w0[d]);
            __m256 w1 = _mm256_loadu_ps(&wx.w1[d]);
            __m256 v = _mm256_fmadd_ps(p1, w1, _mm256_mul_ps(p0, w0));
            _mm256_storeu_ps(out + d, v);
        }
        for (; d < W; ++d) { // tail
            int i = wx.idx[d];
            out[d] = row[i] * wx.w0[d] + row[i + 1] * wx.w1[d];
        }
    }
#else // SSE2 fallback (scalar)
    for (int y = 0; y < srcH; ++y) {
        float const* row = src + y * srcStride;
        float* out = tmp + y * 32;
        for (int d = 0; d < 32; ++d) {
            int i = wx.idx[d];
            out[d] = row[i] * wx.w0[d] + row[i + 1] * wx.w1[d];
        }
    }
#endif
}

// Vertical pass: tmp (srcH × 32) → dst32 (32 × 32)
static void vpass(float const* tmp, int srcH, float* dst, Weights const& wy)
{
    constexpr int W = 32;
#if defined(__AVX2__)
    for (int d = 0; d < W; ++d) {
        float const* col = tmp + d;
        float* out = dst + d;
        int y = 0;
        for (; y + 7 < W; y += 8) {
            __m256 p0 = _mm256_set_ps(
                col[(y + 7) * W + wy.idx[y + 7]],
                col[(y + 6) * W + wy.idx[y + 6]],
                col[(y + 5) * W + wy.idx[y + 5]],
                col[(y + 4) * W + wy.idx[y + 4]],
                col[(y + 3) * W + wy.idx[y + 3]],
                col[(y + 2) * W + wy.idx[y + 2]],
                col[(y + 1) * W + wy.idx[y + 1]],
                col[(y + 0) * W + wy.idx[y + 0]]);

            __m256 p1 = _mm256_set_ps(
                col[(y + 7) * W + wy.idx[y + 7] + W],
                col[(y + 6) * W + wy.idx[y + 6] + W],
                col[(y + 5) * W + wy.idx[y + 5] + W],
                col[(y + 4) * W + wy.idx[y + 4] + W],
                col[(y + 3) * W + wy.idx[y + 3] + W],
                col[(y + 2) * W + wy.idx[y + 2] + W],
                col[(y + 1) * W + wy.idx[y + 1] + W],
                col[(y + 0) * W + wy.idx[y + 0] + W]);

            __m256 w0 = _mm256_loadu_ps(&wy.w0[y]);
            __m256 w1 = _mm256_loadu_ps(&wy.w1[y]);
            __m256 v = _mm256_fmadd_ps(p1, w1, _mm256_mul_ps(p0, w0));
            _mm256_storeu_ps(out + y * W, v);
        }
        for (; y < W; ++y) { // tail
            int j = wy.idx[y];
            out[y * W] = col[j * W] * wy.w0[y] + col[(j + 1) * W] * wy.w1[y];
        }
    }
#else // scalar
    for (int d = 0; d < 32; ++d) {
        float const* col = tmp + d;
        float* out = dst + d;
        for (int y = 0; y < 32; ++y) {
            int j = wy.idx[y];
            out[y * 32] = col[j * 32] * wy.w0[y] + col[(j + 1) * 32] * wy.w1[y];
        }
    }
#endif
}

// Main entry – srcPtr points to luma (float) with stride = srcW
// dst must have at least 32*32 float slots.
static void downscale32x32(float const* src, int srcW, int srcH,
    float* dst)
{
    // (re-)compute coefficient tables if geometry changed
    if (srcW != cacheW || srcH != cacheH) {
        cacheW = srcW;
        cacheH = srcH;
        build_table(srcW, 32, wx);
        build_table(srcH, 32, wy);
    }

    // Scratch buffer: srcH × 32
    static thread_local std::vector<float> tmp;
    tmp.resize(static_cast<size_t>(srcH) * 32);

    hpass(src, srcW, srcH, srcW, tmp.data(), wx);
    vpass(tmp.data(), srcH, dst, wy);
}

} // namespace simd_ds

std::optional<uint64_t>
compute_phash_full(uint8_t const* data, int w, int h)
{
    if (!data || w <= 0 || h <= 0)
        return std::nullopt;

    // Make CImg copy, then 7×7 mean-filter and resize to 32×32
    CImg<float> img(data, w, h, 1, 1);
    auto const& mean7 = kMean7;

    CImg<float> luma;
    if (img.spectrum() >= 3)
        luma = img.get_channels(0, 2).RGBtoYCbCr().channel(0);
    else
        luma = img.get_channel(0);

    luma.convolve(mean7); // smooth first

    float downsized[32 * 32]; // stack buffer
    simd_ds::downscale32x32(luma.data(), luma.width(), luma.height(),
        downsized);

    // wrap downsized data in CImg<float> without copy
    CImg<float> small(downsized, 32, 32, 1, 1, true);

    uint64_t hash = 0;
    if (ph_dct_imagehash_from_buffer(small, hash) < 0)
        return std::nullopt;

    return hash;
}
