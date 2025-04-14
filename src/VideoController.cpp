#include "VideoController.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"

#include <iostream>

VideoController::VideoController(DatabaseManager& db)
    : m_db(db)
{
}

std::vector<VideoInfo> VideoController::gatherVideos(std::filesystem::path const& root)
{
    static std::unordered_set<std::string> exts = { ".mp4", ".webm" };
    auto vids = getVideosFromPath(root, exts);
    return vids;
}

void VideoController::removeAlreadyProcessed(std::vector<VideoInfo>& videos, std::vector<VideoInfo> const& existing)
{
    std::unordered_set<std::string> existingPaths;
    for (auto const& v : existing) {
        existingPaths.insert(v.path);
    }
    std::erase_if(videos, [&](VideoInfo const& v) {
        return existingPaths.contains(v.path);
    });
}

bool VideoController::fillMetadata(VideoInfo& v)
{
    return extract_info(v);
}

void VideoController::storeVideoInDb(VideoInfo& v)
{
    m_db.insertVideo(v);
}

void VideoController::generateScreenshotsAndHashes(std::vector<VideoInfo> const& videos)
{
    for (auto const& v : videos) {

        auto frames = decode_video_frames_as_cimg(v.path);
        if (frames.empty()) {
            std::cerr << "No frames extracted: " << v.path << "\n";
            continue;
        }

        auto hashes = generate_pHashes(frames);
        if (hashes.empty()) {
            std::cerr << "No hashes extracted: " << v.path << "\n";
            continue;
        }
        m_db.insertAllHashes(v.id, hashes);
    }
}

std::vector<std::vector<VideoInfo>> VideoController::detectDuplicates()
{
    auto videos = m_db.getAllVideos();
    auto hashGroups = m_db.getAllHashGroups();

    return findDuplicates(std::move(videos), hashGroups, 4, 5);
}

std::unordered_set<std::string> static const VIDEO_EXTENSIONS = { ".mp4", ".webm" };

void VideoController::runSearchAndDetection(MainWindow& ui)
{
    // 1) Gather videos from filesystem
    std::filesystem::path root("./");
    auto videos = getVideosFromPath(root, { ".mp4", ".webm" });
    auto dbVideos = m_db.getAllVideos();

    // 2) Remove already processed from 'videos'
    {
        std::unordered_set<std::string> knownPaths;
        knownPaths.reserve(dbVideos.size());
        for (auto const& dv : dbVideos) {
            knownPaths.insert(dv.path);
        }

        std::erase_if(videos, [&](VideoInfo const& v) {
            if (knownPaths.contains(v.path)) {
                std::cout << "[Info] Skipping already processed: " << v.path << "\n";
                return true;
            }
            return false;
        });
    }

    // 3) Extract metadata & store in DB
    std::erase_if(videos, [&](VideoInfo& v) {
        if (!extract_info(v)) {
            std::cerr << "Failed to extract media info skipping: " << v.path << "\n";
            return true;
        }
        m_db.insertVideo(v);
        return false;
    });

    // 4) Generate screenshots & pHashes
    for (auto const& v : videos) {
        auto frames = decode_video_frames_as_cimg(v.path);
        if (frames.empty()) {
            std::cerr << "No frames extracted: " << v.path << "\n";
            continue;
        }

        auto hashes = generate_pHashes(frames);
        if (hashes.empty()) {
            std::cerr << "No hashes extracted: " << v.path << "\n";
            continue;
        }
        m_db.insertAllHashes(v.id, hashes);
    }

    // 5) Identify duplicates from DB
    auto allVideos = m_db.getAllVideos();
    auto hashGroups = m_db.getAllHashGroups();

    auto duplicates = findDuplicates(std::move(allVideos),
        hashGroups,
        /*searchRange=*/4,
        /*matchThreshold=*/5);

    ui.setDuplicateVideoGroups(duplicates);
}
