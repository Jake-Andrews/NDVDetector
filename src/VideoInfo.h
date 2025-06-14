#pragma once

#include <string>
#include <vector>

//   path
//   size
//   inode
//   device
//   num_hard_links
struct VideoInfo {
    // set by DB
    int id = 0;

    // set by getVideosFromPath
    std::string path="";        
    int64_t size = 0;
    long inode = 0;
    long device = 0;
    int num_hard_links = 0;

    // set by extract_info
    std::string modified_at="";
    std::string video_codec="";
    std::string audio_codec="";
    std::string pix_fmt=""; 
    std::string profile=""; 
    int level = 0; 
    int width = 0;
    int height = 0;
    int duration = 0;
    int bit_rate = 0;
    int sample_rate_avg = 0;
    double avg_frame_rate = 0.0;

    std::vector<std::string> thumbnail_path=std::vector<std::string>();
};

struct FractionFloat64 {
    double numerator = 0.0;
    double denominator = 0.0;
};

struct StreamInfo {
    std::string codecType="";
    std::string codecName="";
    int width = 0;
    int height = 0;
    int sampleRateAvg = 0;
    FractionFloat64 avgFrameRate;
};

struct FormatInfo {
    std::string duration="";
    std::string size="";
    std::string bitRate="";
};

struct FFProbeOutput {
    std::vector<StreamInfo> streams=std::vector<StreamInfo>();
    FormatInfo format;
};

void print_info(FFProbeOutput const& info);

