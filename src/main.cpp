#include "DecodingFrames.h"
#include "SearchDirectories.h"
#include "VideoInfo.h"
#include <iostream>

static std::unordered_set<std::string> const VIDEO_EXTENSIONS = {
    ".mp4",
    ".webm"
};

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <directory_path>\n";
        return 1;
    }

    std::filesystem::path input_path(argv[1]);
    std::error_code ec;

    if (!std::filesystem::exists(input_path, ec) || !std::filesystem::is_directory(input_path, ec)) {
        std::cerr << "Invalid path: " << input_path << '\n';
        return 1;
    }

    auto const files = get_files_with_extensions(input_path, VIDEO_EXTENSIONS);

    for (auto const& file : files) {
        std::cout << file << '\n';
    }

    auto const video_path = files[0].c_str();
    decode_and_save_video_frames(video_path);

    auto const infoOpt = extract_info(video_path);
    if (!infoOpt) {
        std::cerr << "Failed to extract media info.\n";
        return 1;
    }

    print_info(*infoOpt);

    return 0;
}
