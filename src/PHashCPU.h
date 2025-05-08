#pragma once

#include <optional>
#include <cstdint>

// Forward declaration of CImg class
namespace cimg_library {
template<typename T> class CImg;
}
using namespace cimg_library;

/**
 * Computes a perceptual hash for a 32x32 grayscale image that has already been
 * mean filtered and properly scaled down.
 * 
 * @param gray Pointer to 32x32 grayscale image data (already processed)
 * @param w Width of the image (should be 32)
 * @param h Height of the image (should be 32)
 * @return Optional containing the 64-bit hash, or nullopt if error
 */
std::optional<uint64_t> compute_phash_from_preprocessed(uint8_t const* gray);

/**
 * Computes a perceptual hash for a full-sized image, performing all steps
 * of the pHash algorithm.
 * 
 * @param image CImg object containing the image data
 * @return Optional containing the 64-bit hash, or nullopt if error
 */
std::optional<uint64_t> compute_phash_full(uint8_t const* image, int width, int height);
