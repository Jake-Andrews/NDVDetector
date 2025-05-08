#include "PHashCPU.h"

#include <cstdint>
#include <optional>

#define cimg_use_sqrt
#define cimg_use_math
#include "CImgWrapper.h"

static constexpr int PRE_PROCESSED_SIZE = 32;

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
} // anonymous namespace

std::optional<uint64_t> compute_phash_from_preprocessed(uint8_t const* gray)
{
    // Load raw 8-bit grayscale buffer into CImg
    CImg<float> img(gray, PRE_PROCESSED_SIZE, PRE_PROCESSED_SIZE, 1, 1);

    // Compute DCT
    auto const& C = dct_matrix();
    CImg<float> Ct = C.get_transpose();
    CImg<float> dctImg = C * img * Ct;

    // Extract top-left 8x8 coefficients (skip DC at 0,0)
    auto coeffs = dctImg.crop(1, 1, 8, 8).unroll('x');

    // Compute median of coefficients
    float median = coeffs.median();

    // Generate hash by comparing coefficients to median
    uint64_t hash = 0;
    for (int i = 0; i < 64; ++i) {
        hash |= static_cast<uint64_t>(coeffs(i) > median) << (63 - i);
    }

    return hash;
}

std::optional<uint64_t> compute_phash_full(uint8_t const* image_data, int width, int height)
{
    CImg<float> image(image_data, width, height, 1, 1);

    // Create 7x7 mean filter (all 1's, normalization handled by CImg)
    static CImg<float> const mean_filter(7, 7, 1, 1, 1.0f);

    CImg<float> img;

    // Convert to grayscale (luma) based on image type
    if (image.spectrum() >= 3) {
        // Convert RGB/RGBA to YCbCr and take Y channel (luma)
        // Use get_channels to create a modifiable copy
        CImg<float> rgb = image.get_channels(0, 2).RGBtoYCbCr().channel(0);
        img = rgb.get_convolve(mean_filter);
    } else {
        // Already grayscale, create a modifiable copy first
        CImg<float> gray = image.get_channel(0); // Use get_channel instead of channel
        img = gray.get_convolve(mean_filter);
    }

    // Resize to 32x32 using nearest neighbor (type 1)
    img.resize(32, 32, -100, -100, 1); // 1 = nearest neighbor interpolation

    // Compute DCT
    auto const& C = dct_matrix();
    CImg<float> Ct = C.get_transpose();
    CImg<float> dctImg = C * img * Ct;

    // Extract top-left 8x8 coefficients (skip DC)
    auto coeffs = dctImg.crop(1, 1, 8, 8).unroll('x');

    // Compute median of coefficients
    float median = coeffs.median();

    // Generate hash by comparing coefficients to median
    uint64_t hash = 0;
    for (int i = 0; i < 64; ++i) {
        hash |= static_cast<uint64_t>(coeffs(i) > median) << (63 - i);
    }

    return hash;
}
