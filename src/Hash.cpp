#include "Hash.h"
#include <pHash.h>

#include <iostream>
#include <vector>

std::vector<Hash> generate_pHashes(std::vector<cimg_library::CImg<float>> const& images, uint32_t index)
{
    std::vector<Hash> results;

    for (auto const& img : images) {

        ulong64 hashVal = 0;
        if ((ph_dct_imagehash_from_buffer(img, hashVal)) < 0) {
            std::cerr << "[Warning] Unable to compute pHash for: " << index << "\n";
            continue;
        }

        Hash hr;
        hr.index = index;
        hr.value = static_cast<uint64_t>(hashVal);
        results.push_back(std::move(hr));
    }

    return results;
}

void print_pHashes(std::vector<Hash> const& results)
{
    std::cout << "Computed " << results.size() << " pHash values:\n";
    for (auto const& r : results) {
        std::cout << "  Index: " << r.index << "\n";
        std::cout << " => Hash: " << r.value << "\n";
    }
}
