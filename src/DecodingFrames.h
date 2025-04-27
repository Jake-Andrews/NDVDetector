#pragma once

#include <qobject.h>
#include <string>
#include <vector>

#include "CImgWrapper.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

std::vector<uint64_t> 
extract_phashes_from_video(std::string const& file_path,
                           double skip_percent,
                           int video_duration_sec,
                           AVHWDeviceType hw_backend, 
                           int max_frames = 100);


