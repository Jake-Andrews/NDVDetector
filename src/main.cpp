#include "DatabaseManager.h"
#include "DecodingFrames.h"
#include "Hash.h"
#include "SearchDirectories.h"
#include "VideoInfo.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <unordered_map>
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

    // 1. Validate Directory path
    std::filesystem::path input_path(argv[1]);
    std::error_code ec;

    if (!std::filesystem::exists(input_path, ec)
        || !std::filesystem::is_directory(input_path, ec)) {
        std::cerr << "Invalid path: " << input_path << '\n';
        return 1;
    }

    // 2. Search FS for videos and fill out avaliable 'VideoInfo' fields. Then
    // get 'VideoInfo' from DB and remove from 'videos' if path exists in DB.
    auto videos = get_video_info(input_path, VIDEO_EXTENSIONS);

    DatabaseManager db("videos.db");
    auto const db_videos = db.getAllVideos();

    std::cout << db_videos.size() << "\n";

    remove_already_processed(videos, db_videos);

    std::cout << videos.size() << "\n";

    // 3. Get more video info from FFprobe to use during hashing & screenshoting
    // and to identify corrupt videos to skip
    for (auto& v : videos) {
        std::cout << "Processing: " << v.path << '\n';

        bool success = extract_info(v);
        if (!success) {
            std::cerr << "Failed to extract media info for: " << v.path << "\n";
            // remove from list, mark?
            continue;
        }

        db.insertVideo(v);
    }

    // 4. Generate screenshots and hash them. Then store the hashes and one
    // screenshot in the DB for display in the UI (to do).
    std::unordered_map<int, std::string> indexToPath;

    uint32_t i = 0;
    for (auto const& v : videos) {

        indexToPath[i] = v.path;

        std::string const a = v.path;

        auto screenshots
            = decode_video_frames_as_cimg(a);
        if (screenshots.empty()) {
            std::cerr << "No frames extracted from: " << v.path << "\n";
            continue;
        }
        std::cout << "sneed" << "\n";
        std::cout.flush();

        auto hashResults = generate_pHashes(screenshots, i);
        print_pHashes(hashResults);

        i++;
    }

    // 5. Identify Duplicate videos based on pHashes

    return 0;
}
