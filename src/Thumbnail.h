#pragma once

#include <optional>
#include <vector>
#include <QString>

class VideoInfo;

// Returns the first frame found after seeking with AVSEEK_FLAG_ANY
std::optional<std::vector<QString>>
extract_color_thumbnails(VideoInfo const& info, int thumbnailsToGenerate);

// Returns a frame whose PTS is within ±0.1 s of the target
std::optional<std::vector<QString>>
extract_color_thumbnails_precise(VideoInfo const& info, int thumbnailsToGenerate);

