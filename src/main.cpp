#include "DecodingFrames.h"
#include "Hash.h" // So we see generate_pHashes
#include "SearchDirectories.h"
#include "VideoInfo.h"
#include <iostream>
#include <unordered_set>

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

    if (!std::filesystem::exists(input_path, ec)
        || !std::filesystem::is_directory(input_path, ec)) {
        std::cerr << "Invalid path: " << input_path << '\n';
        return 1;
    }

    auto const files = get_files_with_extensions(input_path, VIDEO_EXTENSIONS);

    for (auto const& video_path : files) {
        std::cout << "Processing video: " << video_path << '\n';

        auto const infoOpt = extract_info(video_path);
        if (!infoOpt) {
            std::cerr << "Failed to extract media info for: " << video_path << "\n";
            continue;
        }
        print_info(*infoOpt);

        auto screenshotPaths = decode_and_save_video_frames(video_path);
        if (screenshotPaths.empty()) {
            std::cerr << "No frames extracted from: " << video_path << "\n";
            continue;
        }

        auto hashResults = generate_pHashes(screenshotPaths);
        print_pHashes(hashResults);
    }

    return 0;
}
