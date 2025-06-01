#pragma once

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <string>
#include <vector>

enum class HashMethod { Fast,
    Slow };

struct FastHashSettings {
    int maxFrames = 2; 
    int hammingDistance = 4;
    std::uint64_t matchingThreshold = 2;
    bool useKeyframesOnly = true;
};

struct SlowHashSettings {
    int skipPercent = 15; // 0-40
    int maxFrames = 2'147'483'647;
    int hammingDistance = 4;
    bool usePercentThreshold = false;
    double matchingThresholdPct = 50.0;     // 1-100
    std::uint64_t matchingThresholdNum = 5; // 1-10000
};

/* json helpers */
inline void to_json(nlohmann::json& j, FastHashSettings const& f)
{
    j = { { "maxFrames", f.maxFrames },
        { "hammingDistance", f.hammingDistance },
        { "matchingThreshold", f.matchingThreshold },
        { "useKeyframesOnly", f.useKeyframesOnly } };
}
inline void from_json(nlohmann::json const& j, FastHashSettings& f)
{
    j.at("maxFrames").get_to(f.maxFrames);
    j.at("hammingDistance").get_to(f.hammingDistance);
    j.at("matchingThreshold").get_to(f.matchingThreshold);
    if (j.contains("useKeyframesOnly"))
        j.at("useKeyframesOnly").get_to(f.useKeyframesOnly);
    else
        f.useKeyframesOnly = true;

    f.maxFrames = (f.maxFrames == 2 || f.maxFrames == 10) ? f.maxFrames : 2; // Only allow 2 or 10
    f.hammingDistance = std::clamp(f.hammingDistance, 0, 64);
    f.matchingThreshold = std::clamp<std::uint64_t>(f.matchingThreshold, 1, 10'000);
}
inline void to_json(nlohmann::json& j, SlowHashSettings const& s)
{
    j = { { "skipPercent", s.skipPercent },
        { "maxFrames", s.maxFrames },
        { "hammingDistance", s.hammingDistance },
        { "usePercentThreshold", s.usePercentThreshold },
        { "matchingThresholdPct", s.matchingThresholdPct },
        { "matchingThresholdNum", s.matchingThresholdNum } };
}
inline void from_json(nlohmann::json const& j, SlowHashSettings& s)
{
    j.at("skipPercent").get_to(s.skipPercent);
    j.at("maxFrames").get_to(s.maxFrames);
    j.at("hammingDistance").get_to(s.hammingDistance);
    j.at("usePercentThreshold").get_to(s.usePercentThreshold);
    j.at("matchingThresholdPct").get_to(s.matchingThresholdPct);
    j.at("matchingThresholdNum").get_to(s.matchingThresholdNum);

    s.skipPercent = std::clamp(s.skipPercent, 0, 40);
    // No clamping for slow mode - user can choose any value
    s.hammingDistance = std::clamp(s.hammingDistance, 0, 64);
    s.matchingThresholdPct = std::clamp(s.matchingThresholdPct, 1.0, 100.0);
    s.matchingThresholdNum = std::clamp<std::uint64_t>(s.matchingThresholdNum, 1, 10'000);
}

extern "C" {
#include <libavutil/hwcontext.h>
}

struct DirectoryEntry {
    std::string path; // absolute, normalised
    bool recursive = true;
};

inline void to_json(nlohmann::json& j, DirectoryEntry const& d)
{
    j = nlohmann::json { { "path", d.path }, { "recursive", d.recursive } };
}

inline void from_json(nlohmann::json const& j, DirectoryEntry& d)
{
    j.at("path").get_to(d.path);
    j.at("recursive").get_to(d.recursive);
}

struct SearchSettings {
    AVHWDeviceType hwBackend { AV_HWDEVICE_TYPE_NONE };

    bool useGlob = false;
    bool caseInsensitive = false;

    std::vector<std::string> extensions = {};
    std::vector<std::string> includeFilePatterns, includeDirPatterns,
        excludeFilePatterns, excludeDirPatterns;
    std::optional<std::uint64_t> minBytes, maxBytes;
    std::vector<DirectoryEntry> directories;

    std::vector<std::regex> includeFileRx, includeDirRx,
        excludeFileRx, excludeDirRx;

    int thumbnailsPerVideo = 4; // 1-4

    HashMethod method = HashMethod::Fast;
    FastHashSettings fastHash;
    SlowHashSettings slowHash;
};

inline void to_json(nlohmann::json& j, SearchSettings const& s)
{
    j = nlohmann::json {
        { "useGlob", s.useGlob },
        { "caseInsensitive", s.caseInsensitive },
        { "extensions", s.extensions },
        { "includeFilePatterns", s.includeFilePatterns },
        { "includeDirPatterns", s.includeDirPatterns },
        { "excludeFilePatterns", s.excludeFilePatterns },
        { "excludeDirPatterns", s.excludeDirPatterns },
        { "directories", s.directories },
        { "thumbnailsPerVideo", s.thumbnailsPerVideo }
    };

    j["minBytes"] = s.minBytes.has_value() ? nlohmann::json(*s.minBytes) : nullptr;
    j["maxBytes"] = s.maxBytes.has_value() ? nlohmann::json(*s.maxBytes) : nullptr;

    j["method"] = static_cast<int>(s.method);
    j["fastHash"] = s.fastHash;
    j["slowHash"] = s.slowHash;
}

inline void from_json(nlohmann::json const& j, SearchSettings& s)
{
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

    if (j.contains("method"))
        s.method = static_cast<HashMethod>(j.at("method").get<int>());
    if (j.contains("fastHash"))
        j.at("fastHash").get_to(s.fastHash);
    if (j.contains("slowHash"))
        j.at("slowHash").get_to(s.slowHash);
}

namespace detail {
inline std::string globToRegex(std::string_view glob)
{
    std::string rx;
    rx.reserve(glob.size() * 2);
    rx += '^';
    for (char c : glob) {
        switch (c) {
        case '*':
            rx += ".*";
            break; // any sequence
        case '?':
            rx += '.';
            break; // single char
        case '.':
            rx += "\\.";
            break; // escape dots
        case '\\':
            rx += "\\\\";
            break; // escape back‑slash
        case '+':
        case '(':
        case ')':
        case '{':
        case '}':
        case '^':
        case '$':
        case '|':
        case '[':
        case ']':
            rx += '\\';
            rx += c;
            break; // escape regex meta
        default:
            rx += c;
        }
    }
    rx += '$';
    return rx;
}
}

inline std::vector<std::regex>
compileRegexList(std::vector<std::string> const& patterns,
    bool useGlob,
    bool icase,
    std::vector<std::string>& errors)
{
    auto const flags = std::regex::ECMAScript | (icase ? std::regex::icase : std::regex::flag_type {});

    std::vector<std::regex> out;
    out.reserve(patterns.size());

    for (auto const& raw : patterns) {
        try {
            std::string rx = useGlob ? detail::globToRegex(raw) : raw;
            out.emplace_back(rx, flags);
        } catch (std::regex_error const& e) {
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
    s.includeDirRx = compileRegexList(s.includeDirPatterns, s.useGlob, s.caseInsensitive, errs);
    s.excludeFileRx = compileRegexList(s.excludeFilePatterns, s.useGlob, s.caseInsensitive, errs);
    s.excludeDirRx = compileRegexList(s.excludeDirPatterns, s.useGlob, s.caseInsensitive, errs);
    return errs;
}

#include <QMetaType>          // NEW
Q_DECLARE_METATYPE(SearchSettings)   // NEW
