#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

std::vector<uint64_t> decode_and_hash(
    std::string const& file,
    int skip_pct,
    int duration_s,
    int max_frames,
    std::function<void(int)> const& on_progress = {});
