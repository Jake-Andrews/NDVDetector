#include "FileSystemSearch.h"
#include "SearchSettings.h"
#include "VideoInfo.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <regex>
#include <spdlog/spdlog.h>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#    include <windows.h>
#elif defined(__unix__)
#    include <sys/stat.h>
#endif

namespace {

#ifdef _WIN32
bool get_file_identity(const std::filesystem::path& path, long& inode, long& device, int& nlinks, std::string& modified_at)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    BY_HANDLE_FILE_INFORMATION fileInfo;
    bool success = GetFileInformationByHandle(hFile, &fileInfo);
    if (!success) {
        CloseHandle(hFile);
        return false;
    }

    inode = (static_cast<uint64_t>(fileInfo.nFileIndexHigh) << 32) | fileInfo.nFileIndexLow;
    device = static_cast<long>(fileInfo.dwVolumeSerialNumber);
    nlinks = static_cast<int>(fileInfo.nNumberOfLinks);

    // Convert last write time (modification time) to SYSTEMTIME
    FILETIME localFileTime;
    if (!FileTimeToLocalFileTime(&fileInfo.ftLastWriteTime, &localFileTime)) {
        CloseHandle(hFile);
        return false;
    }

    SYSTEMTIME systemTime;
    if (!FileTimeToSystemTime(&localFileTime, &systemTime)) {
        CloseHandle(hFile);
        return false;
    }

    // Format SYSTEMTIME to string
    char buffer[20]; // "YYYY-MM-DD HH:MM:SS" + null terminator
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
        systemTime.wYear, systemTime.wMonth, systemTime.wDay,
        systemTime.wHour, systemTime.wMinute, systemTime.wSecond);

    modified_at = buffer;

    CloseHandle(hFile);
    return true;
}
#elif defined(__unix__)
bool get_file_identity(const std::filesystem::path& path, long& inode, long& device, int& nlinks, std::string& modified_at)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;

    inode = static_cast<long>(st.st_ino);
    device = static_cast<long>(st.st_dev);
    nlinks = static_cast<int>(st.st_nlink);

    std::time_t mtime = st.st_mtime;

    // not thread safe
    std::tm* tm = std::localtime(&mtime);

    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
    modified_at = oss.str();

    return true;
}
#else
bool get_file_identity(const std::filesystem::path&, long&, long&, int&)
{
    return false; // Not supported
}
#endif

} // namespace

static bool matchAny(std::string const& text,
    std::vector<std::regex> const& vec)
{
    return std::any_of(vec.begin(), vec.end(),
        [&](auto const& rx) { return std::regex_search(text, rx); });
}

// VideoInfo fields set:
//   path
//   size
//   inode
//   device
//   num_hard_links
std::vector<VideoInfo>
getVideosFromPath(std::filesystem::path const& root, SearchSettings const& cfg)
{
    std::vector<VideoInfo> out;
    std::error_code ec;

    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        spdlog::warn("Invalid root path: {}", root.string());
        return out;
    }

    // Log search filters
    spdlog::info("Searching path: {}", root.string());
    spdlog::info("  Recursive: {}", std::ranges::any_of(cfg.directories, [&](auto const& d) {
        return d.path == root.lexically_normal().string() && d.recursive;
    })
            ? "true"
            : "false");

    spdlog::info("  Extensions:");
    for (auto const& ext : cfg.extensions)
        spdlog::info("    - {}", ext);

    spdlog::info("  Include File Patterns:");
    for (auto const& r : cfg.includeFilePatterns)
        spdlog::info("    - {}", r);

    spdlog::info("  Include Dir Patterns:");
    for (auto const& r : cfg.includeDirPatterns)
        spdlog::info("    - {}", r);

    spdlog::info("  Exclude File Patterns:");
    for (auto const& r : cfg.excludeFilePatterns)
        spdlog::info("    - {}", r);

    spdlog::info("  Exclude Dir Patterns:");
    for (auto const& r : cfg.excludeDirPatterns)
        spdlog::info("    - {}", r);

    spdlog::info("  MinBytes: {}", cfg.minBytes ? std::to_string(*cfg.minBytes) : "None");
    spdlog::info("  MaxBytes: {}", cfg.maxBytes ? std::to_string(*cfg.maxBytes) : "None");

    bool recurse = true;
    for (auto const& d : cfg.directories) {
        if (d.path == root.lexically_normal().string()) {
            recurse = d.recursive;
            break;
        }
    }

    auto dopt = std::filesystem::directory_options::skip_permission_denied;

    auto processEntry = [&](std::filesystem::directory_entry const& entry) {
        if (!entry.is_regular_file())
            return;

        auto const& path = entry.path();
        std::string fname = path.filename().string();
        std::string dir = path.parent_path().string();

        // --- extension test ---
        std::string ext = path.extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (!cfg.extensions.empty() && std::find(cfg.extensions.begin(), cfg.extensions.end(), ext) == cfg.extensions.end()) {
            return;
        }

        // --- size test ---
        std::error_code szErr;
        auto sz = std::filesystem::file_size(path, szErr);
        if (szErr)
            return;

        if (cfg.minBytes && sz < *cfg.minBytes)
            return;
        if (cfg.maxBytes && sz > *cfg.maxBytes)
            return;

        bool incF = matchAny(fname, cfg.includeFileRx);
        bool incD = matchAny(dir, cfg.includeDirRx);
        bool excF = matchAny(fname, cfg.excludeFileRx);
        bool excD = matchAny(dir, cfg.excludeDirRx);

        bool hasInclude = !cfg.includeFileRx.empty() || !cfg.includeDirRx.empty();

        bool accept = false;
        if (hasInclude) {
            // Include filters override everything — exclude filters ignored
            accept = incF || incD;
        } else {
            // No include filters — exclude filters apply
            accept = !(excF || excD);
        }

        if (!accept) {
            spdlog::debug("Filtered: {}", path.string());
            return;
        }

        VideoInfo video;
        std::error_code absErr;
        auto absPath = std::filesystem::absolute(path, absErr);
        if (absErr)
            return;

        video.path = absPath.lexically_normal().string();
        video.size = static_cast<int>(sz);

        if (!get_file_identity(absPath, video.inode, video.device, video.num_hard_links, video.modified_at)) {
            spdlog::debug("Failed to get file identify, rejecting: {}", path.string());
            return;
        }

        spdlog::debug("Accepted: {}", video.path);
        out.push_back(std::move(video));
    };

    if (recurse) {
        for (auto const& entry : std::filesystem::recursive_directory_iterator(root, dopt, ec)) {
            if (ec) {
                spdlog::warn("Recursive iterator error: {}", ec.message());
                continue;
            }
            processEntry(entry);
        }
    } else {
        for (auto const& entry : std::filesystem::directory_iterator(root, dopt, ec)) {
            if (ec) {
                spdlog::warn("Directory iterator error: {}", ec.message());
                continue;
            }
            processEntry(entry);
        }
    }

    return out;
}

bool validate_directory(std::filesystem::path const& root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        std::cerr << "Invalid directory: " << root << (ec ? (" (" + ec.message() + ")") : "") << '\n';
        return false;
    }
    return true;
}
