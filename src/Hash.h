#pragma once

#include <vector>
#include <cstdint>
#include <optional>

#include "CImgWrapper.h"

struct Hash {
    uint64_t value = 0;
    int fk_hash_video = -1;
};

struct HashGroup {
    int fk_hash_video = -1;
    std::vector<uint64_t> hashes;
};

std::vector<uint64_t> generate_pHashes(std::vector<CImg<float>> const&);

void print_pHashes(std::vector<Hash> const& results);

// Input is a 32×32 8-bit gray tile that has already been mean-filtered
// and scaled down.  Returns std::nullopt on error.
std::optional<uint64_t>
compute_phash_from_preprocessed(uint8_t const* gray32x32);

// Convenience variant that performs the full algorithm on an
// arbitrary image buffer (RGB / RGBA / gray).
std::optional<uint64_t>
compute_phash_full(uint8_t const* img, int w, int h);

