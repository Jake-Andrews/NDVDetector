#include "PHashCPU.h"

#define cimg_use_sqrt
#define cimg_use_math
#include "CImgWrapper.h"

namespace {

// Generate (once) the 32×32 DCT transform matrix used by pHash
static CImg<float> make_dct_matrix(int N)
{
    CImg<float> M(N, N, 1, 1, 1.0f / std::sqrt(static_cast<float>(N)));
    float c1 = std::sqrt(2.0f / N);
    for (int x = 0; x < N; ++x)
        for (int y = 1; y < N; ++y)
            M(x, y) = c1 * std::cos((cimg::PI / 2 / N) * y * (2 * x + 1));
    return M;
}

static CImg<float> const& dct_matrix()
{
    static CImg<float> D = make_dct_matrix(32);
    return D;
}

} // namespace anon

std::optional<uint64_t> compute_phash_cpu(uint8_t const* gray, int w, int h)
{
    if (!gray || w <= 0 || h <= 0)
        return std::nullopt;

    // Load raw 8-bit grayscale buffer into CImg
    CImg<float> img(gray, w, h, 1, 1);

    // Apply 7x7 mean filter before scaling to average out differences
    static CImg<float> const mean_filter(7, 7, 1, 1, 1.0f);
    img.convolve(mean_filter);

    // Scaling uses Lanczos
    img.resize(32, 32, -100, -100, 3);

    // DCT
    auto const& C = dct_matrix();
    CImg<float> Ct = C.get_transpose();
    CImg<float> dctImg = C * img * Ct;

    // Extract top-left 8x8 coefficients (skip DC)
    auto coeffs = dctImg.crop(1, 1, 8, 8).unroll('x');

    // SIMD-friendly median and comparison
    float median = coeffs.median();

    uint64_t hash = 0;
    for (int i = 0; i < 64; ++i) {
        hash |= static_cast<uint64_t>(coeffs(i) > median) << (63 - i);
    }

    return hash;
}
