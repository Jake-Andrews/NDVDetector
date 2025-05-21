#pragma once

#include <qobject.h>
#include <string>
#include <vector>
#include "VideoInfo.h"          // new

std::optional<std::vector<QString>>
extract_color_thumbnails(VideoInfo const& info,
                         int thumbnailsToGenerate);
