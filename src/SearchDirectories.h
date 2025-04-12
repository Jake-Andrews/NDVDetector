#pragma once

#include "VideoInfo.h"
#include <filesystem>
#include <unordered_set>
#include <vector>

std::vector<VideoInfo>
get_video_info(std::filesystem::path const& root,
               std::unordered_set<std::string> const& extensions);

