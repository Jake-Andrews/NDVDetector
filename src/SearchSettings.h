#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

struct DirectoryEntry {
    std::string path;            // absolute, normalised
    bool        recursive = true;
};
inline void to_json(nlohmann::json& j, const DirectoryEntry& d) {
    j = {{"path", d.path}, {"recursive", d.recursive}};
}
inline void from_json(const nlohmann::json& j, DirectoryEntry& d) {
    j.at("path").get_to(d.path);
    j.at("recursive").get_to(d.recursive);
}

struct SearchSettings {
    bool useGlob         = false;   // translate * and ? into regex
    bool caseInsensitive = false;   // compile with std::regex::icase

    std::unordered_set<std::string> extensions;           // lower‑case
    std::vector<std::string> includeFilePatterns,
                              includeDirPatterns,
                              excludeFilePatterns,
                              excludeDirPatterns;
    std::optional<std::uint64_t> minBytes, maxBytes;      // inclusive
    std::vector<DirectoryEntry>  directories;

    // serialise *all* public data members 
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SearchSettings,
        useGlob, caseInsensitive,
        extensions,
        includeFilePatterns, includeDirPatterns,
        excludeFilePatterns, excludeDirPatterns,
        minBytes, maxBytes, directories)

    // pre‑compiled regexes (not serialised)
    std::vector<std::regex> includeFileRx, includeDirRx,
                            excludeFileRx, excludeDirRx;
};

namespace detail {
inline std::string globToRegex(std::string_view glob) {
    std::string rx;  rx.reserve(glob.size()*2);  rx += '^';
    for (char c : glob) {
        switch (c) {
        case '*':  rx += ".*"; break;              // any sequence
        case '?':  rx += '.' ; break;              // single char
        case '.':  rx += "\\."; break;             // escape dots
        case '\\': rx += "\\\\"; break;            // escape back‑slash
        case '+': case '(': case ')': case '{': case '}':
        case '^': case '$': case '|': case '[': case ']':
            rx += '\\'; rx += c; break;            // escape regex meta
        default:   rx += c;
        }
    }
    rx += '$';
    return rx;
}
} 

inline std::vector<std::regex>
compileRegexList(const std::vector<std::string>& patterns,
                 bool  useGlob,
                 bool  icase,
                 std::vector<std::string>& errors)         
{
    const auto flags = std::regex::ECMAScript |
                      (icase ? std::regex::icase : std::regex::flag_type{});

    std::vector<std::regex> out;
    out.reserve(patterns.size());

    for (auto const& raw : patterns) {
        try {
            std::string rx = useGlob ? detail::globToRegex(raw) : raw;
            out.emplace_back(rx, flags);                    
        } catch (const std::regex_error& e) {
            errors.emplace_back(raw + "  ➜  " + e.what());   
        }
    }
    return out;
}



inline std::vector<std::string>               
compileAllRegexes(SearchSettings& s)
{
    std::vector<std::string> errs;
    s.includeFileRx = compileRegexList(s.includeFilePatterns, s.useGlob, s.caseInsensitive, errs);
    s.includeDirRx  = compileRegexList(s.includeDirPatterns , s.useGlob, s.caseInsensitive, errs);
    s.excludeFileRx = compileRegexList(s.excludeFilePatterns, s.useGlob, s.caseInsensitive, errs);
    s.excludeDirRx  = compileRegexList(s.excludeDirPatterns , s.useGlob, s.caseInsensitive, errs);
    return errs;
}
