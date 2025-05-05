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

