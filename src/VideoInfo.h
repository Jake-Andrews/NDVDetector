#pragma once

#include <string>
#include <vector>

struct VideoInfo {
    int id = 0;

    std::string path;        
    std::string created_at;  
    std::string modified_at;
    std::string video_codec;
    std::string audio_codec;
    int width = 0;
    int height = 0;
    int duration = 0;
    int size = 0;
    int bit_rate = 0;
    int num_hard_links = 0;
    long inode = 0;
    long device = 0;
    int sample_rate_avg = 0;
    double avg_frame_rate = 0.0;
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

void print_info(FFProbeOutput const& info);

