#include "FileSystemSearch.h"
#include "VideoInfo.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#    include <windows.h>
#elif defined(__unix__)
#    include <sys/stat.h>
#endif

namespace {

#ifdef _WIN32
bool get_file_identity(const std::filesystem::path& path, long& inode, long& device, int& nlinks)
{
    HANDLE hFile = CreateFileW(path.c_str(), 0, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    BY_HANDLE_FILE_INFORMATION fileInfo;
    bool success = GetFileInformationByHandle(hFile, &fileInfo);
    CloseHandle(hFile);

    if (!success)
        return false;

    inode = (static_cast<uint64_t>(fileInfo.nFileIndexHigh) << 32) | fileInfo.nFileIndexLow;
    device = static_cast<long>(fileInfo.dwVolumeSerialNumber);
    nlinks = static_cast<int>(fileInfo.nNumberOfLinks);
    return true;
}
#elif defined(__unix__)
bool get_file_identity(const std::filesystem::path& path, long& inode, long& device, int& nlinks)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;

    inode = static_cast<long>(st.st_ino);
    device = static_cast<long>(st.st_dev);
    nlinks = static_cast<int>(st.st_nlink);
    return true;
}
#else
bool get_file_identity(const std::filesystem::path&, long&, long&, int&)
{
    return false; // Not supported
}
#endif

} // namespace

std::vector<VideoInfo> getVideosFromPath(std::filesystem::path const& root,
    std::unordered_set<std::string> const& extensions)
{
    std::vector<VideoInfo> results;
    std::error_code ec;

    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return results;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
        it != std::filesystem::recursive_directory_iterator();
        it.increment(ec)) {

        if (ec) {
            std::cerr << "[Warning] Skipping due to error: " << ec.message() << "\n";
            continue;
        }

        if (std::filesystem::is_regular_file(*it, ec)) {
            auto ext = it->path().extension().string();
            if (!ext.empty() && extensions.contains(ext)) {
                std::filesystem::path absPath = std::filesystem::absolute(it->path(), ec).lexically_normal();
                if (ec) {
                    std::cerr << "[Warning] Failed to get absolute path: " << ec.message() << "\n";
                    continue;
                }

                VideoInfo video;
                video.path = absPath.string();

                // Get modification time
                auto ftime = std::filesystem::last_write_time(absPath, ec);
                if (!ec) {
                    auto fileTimePoint = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
                    auto sctp = std::chrono::system_clock::to_time_t(fileTimePoint);
                    video.modified_at = std::to_string(sctp);
                }

                video.created_at = "";
                video.size = static_cast<int>(std::filesystem::file_size(absPath, ec));
                if (ec) {
                    video.size = 0;
                }

                video.video_codec.clear();
                video.audio_codec.clear();
                video.width = 0;
                video.height = 0;
                video.duration = 0;
                video.bit_rate = 0;
                video.sample_rate_avg = 0;
                video.avg_frame_rate = 0.0;

                if (!get_file_identity(absPath, video.inode, video.device, video.num_hard_links)) {
                    video.inode = -1;
                    video.device = -1;
                    video.num_hard_links = 1;
                }

                results.push_back(std::move(video));
            }
        }
    }

    return results;
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

