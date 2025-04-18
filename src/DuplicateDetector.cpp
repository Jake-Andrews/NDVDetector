#include "Hash.h"
#include "UnionFind.h"
#include "VideoInfo.h"
#include <hft/hftrie.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*!
 * \brief findDuplicates
 *        Detects duplicate videos based on pHashes. Replicates the existing logic
 *        for HFTrie insertion, RangeSearch, match count, union-find, etc.
 *
 * \param videos        A list of all VideoInfo from DB.
 * \param hashGroups    Each HashGroup has 'fk_hash_video' + 'hashes' for that video.
 * \param searchRange   The Hamming distance threshold for RangeSearchFast.
 * \param matchThreshold The min # of matching hashes needed to consider a video a duplicate.
 *
 * \return A vector of connected components, each is a vector of VideoInfo duplicates.
 */

std::vector<std::vector<VideoInfo>>
findDuplicates(std::vector<VideoInfo> videos,
    std::vector<HashGroup> const& hashGroups,
    uint64_t searchRange,
    int matchThreshold)
{
    // 1) Build HFTrie from all pHashes
    hft::HFTrie trie;
    for (auto const& group : hashGroups) {
        for (auto h : group.hashes) {
            trie.Insert({ group.fk_hash_video, h });
        }
    }

    // 2) Build an id->index map for union-find
    std::unordered_map<int, int> idToIndex;
    idToIndex.reserve(videos.size());
    for (int i = 0; i < static_cast<int>(videos.size()); ++i) {
        idToIndex[videos[i].id] = i;
    }

    // 3) Collect edges in a vector of (indexOfVideoA, indexOfVideoB)
    std::vector<std::pair<int, int>> duplicates;

    // For each HashGroup => do the range search => build match counts => store edges
    for (auto const& group : hashGroups) {
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
            duplicates.push_back({ mainIndex, matchIndex });
        }
    }

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

    return duplicateGroups;
}
