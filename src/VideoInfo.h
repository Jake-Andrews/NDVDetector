#pragma once

#include <optional>
#include <string>
#include <vector>

struct VideoInfo {
    std::string codecType;
    std::string codecName;
    int width = 0;
    int height = 0;
    int sampleRate = 0;
    int64_t duration = 0;
    int64_t size = 0;
    int64_t bitRate = 0;
    double frameRate = 0.0;
};

struct FractionFloat64 {
    double numerator = 0.0;
    double denominator = 0.0;
};

struct StreamInfo {
    std::string codecType;
    std::string codecName;
    int width = 0;
    int height = 0;
    int sampleRateAvg = 0;
    FractionFloat64 avgFrameRate;
};

struct FormatInfo {
    std::string duration;
    std::string size;
    std::string bitRate;
};

struct FFProbeOutput {
    std::vector<StreamInfo> streams;
    FormatInfo format;
};

std::ostream& operator<<(std::ostream& os, const VideoInfo& info);

std::optional<FFProbeOutput> extract_info(std::string_view inputFile);

void print_info(FFProbeOutput const& info);
