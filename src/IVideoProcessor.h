#pragma once

#include "VideoInfo.h"
#include "SearchSettings.h"

#include <vector>
#include <cstdint>

class IVideoProcessor {
public:
    virtual ~IVideoProcessor() = 0;

    virtual std::vector<std::uint64_t>
    decodeAndHash(
        VideoInfo const& video,
        SearchSettings const& cfg) = 0;
};

inline IVideoProcessor::~IVideoProcessor() = default;
