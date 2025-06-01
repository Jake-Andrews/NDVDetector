#pragma once
#include "IVideoProcessor.h"

class SlowVideoProcessor : public IVideoProcessor {
public:
    std::vector<std::uint64_t>
    decodeAndHash(
        VideoInfo const& video,
        SearchSettings const& cfg);
};
