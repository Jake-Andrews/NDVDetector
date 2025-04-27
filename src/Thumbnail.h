#pragma once

#include <qobject.h>
#include <string>

std::optional<QString> extract_color_thumbnail(std::string const& filePath);
