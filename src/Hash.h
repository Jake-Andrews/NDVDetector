#pragma once

#include <vector>
#include <cstdint>

#include "CImgWrapper.h"

struct Hash {
    uint32_t index = -1;
    uint64_t value = 0;
};

std::vector<Hash> generate_pHashes(std::vector<CImg<float>> const&, uint32_t);

void print_pHashes(std::vector<Hash> const& results);

