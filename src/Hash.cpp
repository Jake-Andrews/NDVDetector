#include "Hash.h"
#include <pHash.h>

#include <iostream>
#include <vector>

std::vector<uint64_t> generate_pHashes(std::vector<CImg<float>> const& images)
{
    std::vector<uint64_t> results;

    for (auto const& img : images) {

        uint64_t hashVal = 0;
        if ((ph_dct_imagehash_from_buffer(img, hashVal)) < 0) {
            std::cerr << "[Warning] Unable to compute pHash.\n";
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
