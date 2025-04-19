#pragma once

#include "VideoInfo.h"
#include "SearchSettings.h"
#include <filesystem>
#include <unordered_set>
#include <vector>

std::vector<VideoInfo>
getVideosFromPath(std::filesystem::path const& root,
                  SearchSettings const& cfg);

bool validate_directory(std::filesystem::path const& root);
