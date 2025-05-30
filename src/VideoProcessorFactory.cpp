// VideoProcessorFactory.cpp
#include "VideoProcessorFactory.h"
#include "FastVideoProcessor.h"
#include "SlowVideoProcessor.h"

std::unique_ptr<IVideoProcessor>
makeVideoProcessor(SearchSettings const& cfg)
{
    using enum HashMethod;
    if (cfg.method == Fast)
        return std::make_unique<FastVideoProcessor>();
    return std::make_unique<SlowVideoProcessor>();
}
