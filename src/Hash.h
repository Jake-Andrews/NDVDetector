#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct HashResult {
    std::string filename;
    uint64_t hashValue = 0;
};

std::vector<HashResult> generate_pHashes(std::vector<std::string> const&);

void print_pHashes(std::vector<HashResult> const& results);

