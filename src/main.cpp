#include "DatabaseManager.h"
#include "DecodingFrames.h"
#include "Hash.h"
#include "SearchDirectories.h"
#include "VideoInfo.h"

#include <cstdio>
#include <cstdlib>
#include <hft/hftrie.hpp>
#include <iostream>
#include <ostream>
#include <unordered_map>
#include <unordered_set>

static std::unordered_set<std::string> const VIDEO_EXTENSIONS = {
    ".mp4",
    ".webm"
};

constexpr uint64_t SEARCH_RANGE = 4;
constexpr int MATCH_THRESHOLD = 5;

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

        // adds id to VideoInfo
        db.insertVideo(v);
    }

    // 4. Generate screenshots and hash them. Then store the hashes and one
    // screenshot in the DB for display in the UI (to do).

    for (auto const& v : videos) {

        auto screenshots
            = decode_video_frames_as_cimg(v.path);
        if (screenshots.empty()) {
            std::cerr << "No frames extracted from: " << v.path << ", skipping.\n";
            continue;
        }

        auto hashes = generate_pHashes(screenshots);
        if (hashes.empty()) {
            std::cerr << "No hashes extracted from: " << v.path << ", skipping.\n";
            continue;
        }

        db.insertAllHashes(v.id, hashes);
    }

    // 5. Identify Duplicate videos based on pHashes
    auto groups = db.getAllHashGroups();
    for (auto const& group : groups) {
        std::cout << "Video ID: " << group.fk_hash_video << ", Hashes: ";
        for (auto h : group.hashes) {
            std::cout << std::hex << h << " ";
        }
        std::cout << "\n";
    }

    hft::HFTrie trie;

    for (auto const& group : groups) {
        for (auto h : group.hashes) {
            trie.Insert({ group.fk_hash_video, h });
        }
    }

    auto const fvideos = db.getAllVideos();

    // fk_hash_video -> Path map
    std::unordered_map<int, std::string> videoPaths;
    videoPaths.reserve(fvideos.size());
    for (auto const& v : fvideos) {
        videoPaths[v.id] = v.path;
    }

    for (auto const& group : groups) {
        std::unordered_map<int, int> matchCounts;

        // For each hash, do a range search, increment match counts
        for (auto h : group.hashes) {
            auto results = trie.RangeSearchFast(h, SEARCH_RANGE);
            for (auto const& r : results) {
                matchCounts[r.id]++;
            }
        }

        // Collect likely matches that appear at least MATCH_THRESHOLD times
        std::unordered_set<int> likelyMatches;
        for (auto const& [videoId, count] : matchCounts) {
            if (count >= MATCH_THRESHOLD && videoId != group.fk_hash_video) {
                likelyMatches.insert(videoId);
            }
        }

        if (!likelyMatches.empty()) {
            auto it = videoPaths.find(group.fk_hash_video);
            if (it != videoPaths.end()) {
                std::cout << "Video: " << it->second << " matches with:\n";
            } else {
                std::cout << "Unknown video id " << group.fk_hash_video << " matches with:\n";
            }

            for (auto matchId : likelyMatches) {
                auto mp = videoPaths.find(matchId);
                if (mp != videoPaths.end()) {
                    std::cout << "   -> " << mp->second << "\n";
                } else {
                    std::cout << "   -> Unknown video id " << matchId << "\n";
                }
            }
        }
    }

    return 0;
}
