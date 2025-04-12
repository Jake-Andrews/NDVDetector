#include "SearchDirectories.h"
#include "VideoInfo.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#ifdef __unix__
#    include <sys/stat.h>
#endif

std::vector<VideoInfo>
get_video_info(std::filesystem::path const& root,
    std::unordered_set<std::string> const& extensions)
{
    std::vector<VideoInfo> results;
    std::error_code ec;

    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return results;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             ec);
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

                // Try to get last_write_time as a Unix timestamp
                auto ftime = std::filesystem::last_write_time(absPath, ec);
                if (!ec) {
                    // You need C++20 for clock_cast; otherwise use a manual cast
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

                // Defaults for now; set later after FFmpeg analysis
                video.video_codec.clear();
                video.audio_codec.clear();
                video.width = 0;
                video.height = 0;
                video.duration = 0;
                video.bit_rate = 0;
                video.sample_rate_avg = 0;
                video.avg_frame_rate = 0.0;

#ifdef __unix__
                struct stat st;
                if (stat(absPath.c_str(), &st) == 0) {
                    video.inode = static_cast<long>(st.st_ino);
                    video.device = static_cast<long>(st.st_dev);
                    video.num_hard_links = static_cast<int>(st.st_nlink);
                }
#endif
                results.push_back(std::move(video));
            }
        }
    }

    return results;
}
