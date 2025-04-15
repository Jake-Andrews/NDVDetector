#pragma once

#include <qobject.h>
#include <string>
#include <vector>

#include "CImgWrapper.h"

std::vector<CImg<float>> decode_video_frames_as_cimg(std::string const& file_path);
std::optional<QString> extract_color_thumbnail(std::string const& filePath);
