#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

std::vector<uint64_t> extract_phashes_from_video(
    std::string const& file,
    double skip_pct,
    int duration_s,
    int max_frames,
    bool allow_hw,
    std::function<void(int)> const& on_progress);

std::vector<uint64_t> decode_and_hash_hw_gl(std::string const& file,
    double skip_pct,
    int duration_s,
    int max_frames,
    std::function<void(int)> const& progress = {});


std::vector<uint64_t> decode_and_hash_sw(
    std::string const& file,
    double skip_pct,
    int duration_s,
    int max_frames,
    std::function<void(int)> const& on_progress = {});
