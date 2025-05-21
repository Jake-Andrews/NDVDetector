#include "Hash.h"
#include "UnionFind.h"
#include "VideoInfo.h"
#include <hft/hftrie.hpp>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*!
 * \brief findDuplicates
 *        Detects duplicate videos based on pHashes.
 *
 * \param videos        A list of all VideoInfo from DB.
 * \param hashGroups    Each HashGroup has 'fk_hash_video' + 'hashes' for that video.
 * \param searchRange   The Hamming distance threshold for RangeSearchFast.
 * \param matchThreshold The minimum number of matching hashes needed to consider a video a duplicate.
 *
 * \return A vector of vector's each containing VideoInfo duplicates.
 */

namespace {
bool g_duplicateDebugEnabled = true;
}

// Call this from outside to enable/disable extra logging.
void setDuplicateDetectorDebug(bool enable) { g_duplicateDebugEnabled = enable; }

std::vector<std::vector<VideoInfo>>
findDuplicates(std::vector<VideoInfo> videos,
    std::vector<HashGroup> const& hashGroups,
    uint64_t searchRange,
    int matchThreshold)
{
    if (g_duplicateDebugEnabled)
        spdlog::info("[DuplicateDetector] start: videos={}, hashGroups={}",
            videos.size(), hashGroups.size());

    // 1) Build HFTrie from all pHashes
    hft::HFTrie trie;
    for (auto const& group : hashGroups) {
        for (auto h : group.hashes) {
            trie.Insert({ group.fk_hash_video, h });
            if (g_duplicateDebugEnabled)
                spdlog::debug("[DuplicateDetector] vid={}  pHash={:016x}",
                    group.fk_hash_video, h);
        }
    }

    // 2) Build an id->index map for union-find
    std::unordered_map<int, int> idToIndex;
    idToIndex.reserve(videos.size());
    for (int i = 0; i < static_cast<int>(videos.size()); ++i) {
        idToIndex[videos[i].id] = i;
    }

    if (g_duplicateDebugEnabled)
        spdlog::info("[DuplicateDetector] built id→index map ({} entries)", idToIndex.size());

    // 3) Collect edges in a vector of (indexOfVideoA, indexOfVideoB)
    std::vector<std::pair<int, int>> duplicates;

    // For each HashGroup => do the range search => build match counts => store edges
    for (auto const& group : hashGroups) {
        if (g_duplicateDebugEnabled) {
            std::string hashesStr;
            for (auto h : group.hashes)
                hashesStr += fmt::format("{:016x} ", h);
            spdlog::info("[DuplicateDetector] processing vid={} hashes=[{}]",
                group.fk_hash_video, hashesStr);
        }

        std::unordered_map<int, int> matchCounts;

        for (auto h : group.hashes) {
            auto results = trie.RangeSearchFast(h, searchRange);
            for (auto const& r : results) {
                // 'r.id' is the fk_hash_video from insertion
                matchCounts[r.id]++;
            }
        }

        // find videos that appear >= matchThreshold times
        std::unordered_set<int> likelyMatches;
        for (auto const& [videoId, count] : matchCounts) {
            if (count >= matchThreshold && videoId != group.fk_hash_video) {
                likelyMatches.insert(videoId);
            }
        }

        // store edges in duplicates vector for union-find
        // group.fk_hash_video is the "primary" video, each matchId is a duplicate
        int mainIndex = idToIndex[group.fk_hash_video];
        for (auto matchId : likelyMatches) {
            int matchIndex = idToIndex[matchId];
            if (g_duplicateDebugEnabled)
                spdlog::info("[DuplicateDetector] duplicate edge {} ↔ {}",
                    group.fk_hash_video, matchId);
            duplicates.push_back({ mainIndex, matchIndex });
        }
    }

    if (g_duplicateDebugEnabled)
        spdlog::info("[DuplicateDetector] total duplicate edges={}", duplicates.size());

    // 4) Create union-find for all videos
    UnionFind uf(static_cast<int>(videos.size()));
    // unify duplicates
    for (auto const& [i, j] : duplicates) {
        uf.unite(i, j);
    }

    // 5) Build connected components => map root -> list of indexes
    std::unordered_map<int, std::vector<int>> comps;
    comps.reserve(videos.size());
    for (int i = 0; i < static_cast<int>(videos.size()); ++i) {
        int root = uf.find(i);
        comps[root].push_back(i);
    }

    // 6) Convert each connected component to a vector of VideoInfo
    std::vector<std::vector<VideoInfo>> duplicateGroups;
    duplicateGroups.reserve(comps.size());
    for (auto& [root, members] : comps) {
        std::vector<VideoInfo> grp;
        grp.reserve(members.size());
        for (auto idx : members) {
            grp.push_back(videos[idx]);
        }
        duplicateGroups.push_back(std::move(grp));
    }

    if (g_duplicateDebugEnabled) {
        spdlog::info("[DuplicateDetector] duplicate groups formed={}", duplicateGroups.size());
        for (size_t i = 0; i < duplicateGroups.size(); ++i) {
            std::string ids;
            for (auto const& v : duplicateGroups[i])
                ids += fmt::format("{} ", v.id);
            spdlog::info("[DuplicateDetector] group #{} -> [{}]", i, ids);
        }
    }

    return duplicateGroups;
}
