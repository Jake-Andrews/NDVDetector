#pragma once

#include <string>
#include <vector>

#include "CImgWrapper.h"

std::vector<CImg<float>> decode_video_frames_as_cimg(std::string const& file_path);
