#include "VideoInfo.h"
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

void print_info(FFProbeOutput const& info)
{
    std::cout << "Format:\n";
    std::cout << "  Duration: " << info.format.duration << " seconds\n";
    std::cout << "  Size: " << info.format.size << " bytes\n";
    std::cout << "  BitRate: " << info.format.bitRate << " b/s\n";

    for (auto const& s : info.streams) {
        std::cout << "Stream:\n";
        std::cout << "  CodecType: " << s.codecType << '\n';
        std::cout << "  CodecName: " << s.codecName << '\n';

        if (s.codecType == "video") {
            std::cout << "  Width: " << s.width << '\n';
            std::cout << "  Height: " << s.height << '\n';
            std::cout << "  AvgFrameRate: " << s.avgFrameRate.numerator << '/'
                      << s.avgFrameRate.denominator << '\n';
        }

        if (s.codecType == "audio") {
            std::cout << "  SampleRateAvg: " << s.sampleRateAvg << '\n';
        }
    }
}
