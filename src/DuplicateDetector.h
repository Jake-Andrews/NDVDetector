#pragma once

#include <vector>
#include "Hash.h"
#include "VideoInfo.h"
#include <hft/hftrie.hpp>

std::vector<std::vector<VideoInfo>>
findDuplicates(std::vector<VideoInfo> videos,
    std::vector<HashGroup> const& hashGroups,
    uint64_t searchRange,
    int matchThreshold);
