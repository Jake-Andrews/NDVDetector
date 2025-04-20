#pragma once

#include <qobject.h>
#include <string>
#include <vector>

#include "CImgWrapper.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

std::vector<cimg_library::CImg<float>>
decode_video_frames_as_cimg(std::string const& file_path,
    double skip_percent,
    int video_duration_sec,
    AVHWDeviceType hwBackend,
    int max_frames = 100);

std::optional<QString> extract_color_thumbnail(std::string const& filePath);
