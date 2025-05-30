#pragma once
#include <vector>
#include <cstdint>
#include "VideoInfo.h"
#include "SearchSettings.h"

class IVideoProcessor {
public:
    virtual ~IVideoProcessor() = default;

    virtual std::vector<std::uint64_t>
    decodeAndHash(
        VideoInfo const& video,
        SearchSettings const& cfg,
        std::function<void(int)> const& on_progress) = 0;
};
