#pragma once

#include "VideoInfo.h"
#include <optional>
#include <QString>

bool extract_info(VideoInfo &v);

std::optional<std::tuple<QString, QString, QString, int>>
probe_video_codec_info(QString const& qpath);
