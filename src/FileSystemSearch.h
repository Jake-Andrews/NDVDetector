#pragma once

#include "VideoInfo.h"
#include <filesystem>
#include <unordered_set>
#include <vector>

std::vector<VideoInfo>
getVideosFromPath(std::filesystem::path const& root,
    std::unordered_set<std::string> const& extensions);

bool validate_directory(std::filesystem::path const& root);
