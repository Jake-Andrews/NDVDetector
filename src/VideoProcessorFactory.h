#pragma once
#include <memory>
#include "IVideoProcessor.h"
#include "SearchSettings.h"

std::unique_ptr<IVideoProcessor>
makeVideoProcessor(SearchSettings const& cfg);
