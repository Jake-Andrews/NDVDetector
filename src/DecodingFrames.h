#pragma once

#include <qobject.h>
#include <string>
#include <vector>

#include "CImgWrapper.h"

std::vector<CImg<float>> decode_video_frames_as_cimg(
    std::string const& file_path,
    double skip_percent,
    double video_duration_sec);
std::optional<QString> extract_color_thumbnail(std::string const& filePath);
