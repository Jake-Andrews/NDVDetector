#include "VideoController.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include "VideoModel.h"
#include <QDebug>
#include <algorithm>
#include <iostream>
#include <numeric>

VideoController::VideoController(DatabaseManager& db, QObject* parent)
    : QObject(parent)
    , m_db(db)
{
}

void VideoController::runSearchAndDetection()
{
    std::filesystem::path root("./");
    auto videos = getVideosFromPath(root, { ".mp4", ".webm" });
    auto dbVideos = m_db.getAllVideos();

    // remove already processed
    {
        std::unordered_set<std::string> knownPaths;
        knownPaths.reserve(dbVideos.size());
        for (auto const& dv : dbVideos) {
            knownPaths.insert(dv.path);
        }
        std::erase_if(videos, [&](VideoInfo const& v) {
            if (knownPaths.contains(v.path)) {
                std::cout << "[Info] Skipping processed: " << v.path << "\n";
                return true;
            }
            return false;
        });
    }

    // Extract metadata + store in db
    std::erase_if(videos, [&](VideoInfo& v) {
        if (!extract_info(v)) {
            std::cerr << "Failed to extract info for: " << v.path << "\n";
            return true;
        }
        m_db.insertVideo(v);
        return false;
    });

    // screenshots + pHashes
    for (auto const& v : videos) {
        auto frames = decode_video_frames_as_cimg(v.path);
        if (frames.empty())
            continue;

        auto hashes = generate_pHashes(frames);
        if (hashes.empty())
            continue;

        m_db.insertAllHashes(v.id, hashes);
    }

    // find duplicates from full db
    auto allVideos = m_db.getAllVideos();
    auto hashGroups = m_db.getAllHashGroups();

    m_currentGroups = findDuplicates(std::move(allVideos), hashGroups, 4, 5);

    // now broadcast them to the view
    emit duplicateGroupsUpdated(m_currentGroups);
}

void VideoController::handleSelectOption(QString const& option)
{
    if (m_currentGroups.empty()) {
        std::cerr << "[Warning] No groups loaded.\n";
        return;
    }

    if (!m_model) {
        std::cerr << "[Warning] No model set.\n";
        return;
    }

    if (option == "exceptLargest") {
        m_model->selectAllExceptLargest();
    } else if (option == "exceptSmallest") {
        m_model->selectAllExceptSmallest();
    }
}

void VideoController::handleSortOption(QString const& option)
{
    if (!m_model)
        return;

    if (option == "size") {
        m_model->sortVideosWithinGroupsBySize();
    } else if (option == "createdAt") {
        m_model->sortVideosWithinGroupsByCreatedAt();
    }
}

void VideoController::handleSortGroupsOption(QString const& option)
{
    if (!m_model)
        return;

    if (option == "size") {
        m_model->sortGroupsBySize();
    } else if (option == "createdAt") {
        m_model->sortGroupsByCreatedAt();
    }
}

void VideoController::handleDeleteOption(QString const& option)
{
    if (!m_model)
        return;

    if (option == "list") {
        m_model->deleteSelectedVideos();
    }
}

void VideoController::setModel(VideoModel* model)
{
    if (!model) {
        qWarning() << "Attempted to set nullptr model!";
        return;
    }
    m_model = model;
}
