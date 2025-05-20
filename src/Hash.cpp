#include "Hash.h"

#include <iostream>
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
