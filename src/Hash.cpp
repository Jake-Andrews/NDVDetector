#include "Hash.h"
#include <pHash.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

std::vector<HashResult> generate_pHashes(std::vector<std::string> const& filepaths)
{
    std::vector<HashResult> results;
    for (auto const& path : filepaths) {
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        ulong64 hashVal = 0;
        if (ph_dct_imagehash(path.c_str(), hashVal) < 0) {
            std::cerr << "[Warning] Unable to compute pHash for: " << path << "\n";
            continue;
        }
        HashResult hr;
        hr.filename = std::filesystem::path(path).filename().string();
        hr.hashValue = static_cast<uint64_t>(hashVal);
        results.push_back(std::move(hr));
    }

    std::sort(results.begin(), results.end(),
        [](auto const& a, auto const& b) {
            return a.filename < b.filename;
        });

    return results;
}

void print_pHashes(std::vector<HashResult> const& results)
{
    std::cout << "Computed " << results.size() << " pHash values:\n";
    for (auto const& r : results) {
        std::cout << "  File: " << r.filename
                  << " => Hash: " << r.hashValue << "\n";
    }
}
