#pragma once

#include <vector>
#include <cstdint>

#include "CImgWrapper.h"

struct Hash {
    int id = -1; 
    uint64_t value = 0;
    int fk_hash_video = -1;
};

std::vector<uint64_t> generate_pHashes(std::vector<CImg<float>> const&);

void print_pHashes(std::vector<Hash> const& results);

