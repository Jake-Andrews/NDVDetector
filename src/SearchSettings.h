#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <string>
#include <vector>
#include <algorithm>

extern "C" {
#include <libavutil/hwcontext.h>
}


struct DirectoryEntry {
    std::string path;           // absolute, normalised
    bool        recursive = true;
};

inline void to_json(nlohmann::json& j, const DirectoryEntry& d) { 
    j = nlohmann::json{{"path", d.path}, {"recursive", d.recursive}}; 
}

inline void from_json(const nlohmann::json& j, DirectoryEntry& d) { 
    j.at("path").get_to(d.path); 
    j.at("recursive").get_to(d.recursive); 
}

struct SearchSettings {
    AVHWDeviceType hwBackend { AV_HWDEVICE_TYPE_NONE };

    bool useGlob         = false;
    bool caseInsensitive = false;

    std::vector<std::string> extensions = {".mp4", ".mkv", ".webm"};
    std::vector<std::string> includeFilePatterns, includeDirPatterns,
                             excludeFilePatterns, excludeDirPatterns;
    std::optional<std::uint64_t> minBytes, maxBytes;
    std::vector<DirectoryEntry>  directories;

    std::vector<std::regex> includeFileRx, includeDirRx,
    excludeFileRx, excludeDirRx;

    // --- hashing related ---
    int thumbnailsPerVideo = 4; // 1-4 
    int skipPercent = 15; // 0-40
    int maxFrames = 2147483647; // 10-2147483647
    int hammingDistanceThreshold = 4; // 0-64

    bool usePercentThreshold = false; // radio-button state
    double matchingThresholdPercent = 50.0; // 1-100
    std::uint64_t matchingThresholdNumber = 5; // 1-1000
};

inline void to_json(nlohmann::json& j, const SearchSettings& s) {
    j = nlohmann::json{
        {"useGlob", s.useGlob},
        {"caseInsensitive", s.caseInsensitive},
        {"extensions", s.extensions},
        {"includeFilePatterns", s.includeFilePatterns},
        {"includeDirPatterns", s.includeDirPatterns},
        {"excludeFilePatterns", s.excludeFilePatterns},
        {"excludeDirPatterns", s.excludeDirPatterns},
        {"directories", s.directories},
        {"thumbnailsPerVideo", s.thumbnailsPerVideo},
        {"usePercentThreshold", s.usePercentThreshold},
        {"matchingThresholdPercent", s.matchingThresholdPercent},
        {"matchingThresholdNumber", s.matchingThresholdNumber},
        {"hammingDistanceThreshold", s.hammingDistanceThreshold}
    };

    j["minBytes"] = s.minBytes.has_value() ? nlohmann::json(*s.minBytes) : nullptr;
    j["maxBytes"] = s.maxBytes.has_value() ? nlohmann::json(*s.maxBytes) : nullptr;
}

inline void from_json(const nlohmann::json& j, SearchSettings& s) {
    j.at("useGlob").get_to(s.useGlob);
    j.at("caseInsensitive").get_to(s.caseInsensitive);
    j.at("extensions").get_to(s.extensions);
    j.at("includeFilePatterns").get_to(s.includeFilePatterns);
    j.at("includeDirPatterns").get_to(s.includeDirPatterns);
    j.at("excludeFilePatterns").get_to(s.excludeFilePatterns);
    j.at("excludeDirPatterns").get_to(s.excludeDirPatterns);
    j.at("directories").get_to(s.directories);

    if (j.contains("thumbnailsPerVideo"))
        j.at("thumbnailsPerVideo").get_to(s.thumbnailsPerVideo);
    else
        s.thumbnailsPerVideo = 4;
    s.thumbnailsPerVideo = std::clamp(s.thumbnailsPerVideo, 1, 4);

    if (j.contains("minBytes") && !j.at("minBytes").is_null())
        s.minBytes = j.at("minBytes").get<std::uint64_t>();
    else
        s.minBytes = std::nullopt;

    if (j.contains("maxBytes") && !j.at("maxBytes").is_null())
        s.maxBytes = j.at("maxBytes").get<std::uint64_t>();
    else
        s.maxBytes = std::nullopt;

    if (j.contains("usePercentThreshold"))
        j.at("usePercentThreshold").get_to(s.usePercentThreshold);
    else
        s.usePercentThreshold = false;

    if (j.contains("matchingThresholdPercent"))
        j.at("matchingThresholdPercent").get_to(s.matchingThresholdPercent);
    else
        s.matchingThresholdPercent = 50.0;

    s.matchingThresholdPercent = std::clamp(s.matchingThresholdPercent, 1.0, 100.0);

    if (j.contains("matchingThresholdNumber"))
        j.at("matchingThresholdNumber").get_to(s.matchingThresholdNumber);
    else
        s.matchingThresholdNumber = 5;

    s.matchingThresholdNumber =
        std::clamp<std::uint64_t>(s.matchingThresholdNumber, 1, 10000);

    if (j.contains("hammingDistanceThreshold"))
        j.at("hammingDistanceThreshold").get_to(s.hammingDistanceThreshold);
    else
        s.hammingDistanceThreshold = 4;

    s.hammingDistanceThreshold = std::clamp(s.hammingDistanceThreshold, 0, 64);
}

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
