#include "DatabaseManager.h"
#include "DecodingFrames.h"
#include "Hash.h"
#include "SearchDirectories.h"
#include "UnionFind.h"
#include "VideoInfo.h"

#include <cstdio>
#include <cstdlib>
#include <hft/hftrie.hpp>
#include <iostream>
#include <ostream>
#include <unordered_map>
#include <unordered_set>

#include "MainWindow.h"
#include <QApplication>

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
    // get 'VideoInfo' from DB and remove from FS 'videos' if path exists in DB.
    auto videos = get_video_info(input_path, VIDEO_EXTENSIONS);

    DatabaseManager db("videos.db");
    auto const db_videos = db.getAllVideos();

    std::cout << db_videos.size() << "\n";

    remove_already_processed(videos, db_videos);

    std::cout << videos.size() << "\n";

    // 3. Get more video info from FFprobe to use during hashing & screenshoting
    // and to identify corrupt videos to skip. Also add videos to DB.

    std::cout << "Extracting media info...\n";

    std::erase_if(videos, [&](VideoInfo& v) {
        std::cout << "Processing: " << v.path << '\n';

        if (!extract_info(v)) {
            std::cerr << "Failed to extract media info skipping: " << v.path << "\n";
            return true;
        }

        // Also modifies v by setting it's id
        db.insertVideo(v);
        return false;
    });

    // 4. Generate screenshots and hash them. Then store the hashes and one
    // screenshot in the DB for display in the UI.
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

    // translating video IDs to an index [0..n-1].
    // needed for union-find on a 0-based index
    std::unordered_map<int, int> idToIndex;
    idToIndex.reserve(fvideos.size());
    for (int i = 0; i < static_cast<int>(fvideos.size()); ++i) {
        idToIndex[fvideos[i].id] = i;
    }

    // ID->path map for logging
    std::unordered_map<int, std::string> videoPaths;
    videoPaths.reserve(fvideos.size());
    for (auto const& v : fvideos) {
        videoPaths[v.id] = v.path;
    }

    // collect edges (pairs) of duplicates to feed UnionFind
    //    i.e. (indexOfVideoA, indexOfVideoB)
    std::vector<std::pair<int, int>> duplicates;

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

        // debug info
        /*
        if (!likelyMatches.empty()) {
            auto it = videoPaths.find(group.fk_hash_video);
            if (it != videoPaths.end()) {
                std::cout << "Video: " << it->second << " matches with:\n";
            } else {
                std::cout << "Unknown video id " << group.fk_hash_video << " matches with:\n";
            }

            for (auto matchId : likelyMatches) {
                if (auto mp = videoPaths.find(matchId); mp != videoPaths.end()) {
                    std::cout << "   -> " << mp->second << "\n";
                } else {
                    std::cout << "   -> Unknown video id " << matchId << "\n";
                }
            }
        }
        */

        //    Store edges in duplicates vector for union-find
        //    group.fk_hash_video is the "primary" video, each matchId is a duplicate
        auto mainIndex = idToIndex[group.fk_hash_video];
        for (auto matchId : likelyMatches) {
            auto matchIndex = idToIndex[matchId];
            duplicates.push_back({ mainIndex, matchIndex });
        }
    }

    // unify duplicates with UnionFind
    UnionFind uf(static_cast<int>(fvideos.size()));
    for (auto const& [i, j] : duplicates) {
        uf.unite(i, j);
    }

    // Build connected components => vector of vector of indexes
    std::unordered_map<int, std::vector<int>> comps;
    comps.reserve(fvideos.size());
    for (int i = 0; i < static_cast<int>(fvideos.size()); ++i) {
        int r = uf.find(i); // find the root
        comps[r].push_back(i);
    }

    // convert to vector-of-vector
    std::vector<std::vector<int>> groupsOfIndexes;
    groupsOfIndexes.reserve(comps.size());
    for (auto& [root, members] : comps) {
        groupsOfIndexes.push_back(std::move(members));
    }

    // Convert each groupOfIndexes => actual vector of VideoInfo
    std::vector<std::vector<VideoInfo>> duplicateGroups;
    duplicateGroups.reserve(groupsOfIndexes.size());
    for (auto const& comp : groupsOfIndexes) {
        std::vector<VideoInfo> grp;
        grp.reserve(comp.size());
        for (auto idx : comp) {
            grp.push_back(fvideos[idx]);
        }
        duplicateGroups.push_back(std::move(grp));
    }

    QApplication app(argc, argv);
    MainWindow w;

    w.setDuplicateVideoGroups(duplicateGroups);

    w.show();
    return app.exec();
}
