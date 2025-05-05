#pragma once
#include <cstdint>
#include <optional>

// Compute a 64‑bit perceptual hash from a 32×32 8‑bit grayscale buffer.
std::optional<uint64_t> compute_phash_cpu(uint8_t const* gray, int w, int h);
