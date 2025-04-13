#include "UnionFind.h"
#include <algorithm>
#include <unordered_map>

UnionFind::UnionFind(int n)
    : parent(n)
    , rank(n, 0)
{
    for (int i = 0; i < n; ++i)
        parent[i] = i;
}

int UnionFind::find(int x)
{
    if (parent[x] != x)
        parent[x] = find(parent[x]);
    return parent[x];
}

void UnionFind::unite(int x, int y)
{
    int rx = find(x), ry = find(y);
    if (rx != ry) {
        if (rank[rx] < rank[ry])
            std::swap(rx, ry);
        parent[ry] = rx;
        if (rank[rx] == rank[ry])
            rank[rx]++;
    }
}

std::vector<std::vector<int>> buildDuplicateGroups(
    int numVideos,
    std::vector<std::pair<int, int>> const& duplicates)
{
    UnionFind uf(numVideos);
    for (auto const& [a, b] : duplicates) {
        uf.unite(a, b);
    }

    std::unordered_map<int, std::vector<int>> comps;
    comps.reserve(numVideos);
    for (int i = 0; i < numVideos; ++i) {
        int r = uf.find(i);
        comps[r].push_back(i);
    }

    std::vector<std::vector<int>> groups;
    groups.reserve(comps.size());
    for (auto& kv : comps) {
        groups.push_back(std::move(kv.second));
    }
    return groups;
}
