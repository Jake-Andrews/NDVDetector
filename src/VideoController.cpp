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

void VideoController::handleSelectOption(MainWindow::SelectOptions option)
{
    if (!m_model)
        return;

    switch (option) {
    case MainWindow::SelectOptions::AllExceptLargest:
        m_model->selectAllExceptLargest();
        break;
    case MainWindow::SelectOptions::AllExceptSmallest:
        m_model->selectAllExceptSmallest();
        break;
    default:
        qWarning() << "Unhandled SelectOptions";
        break;
    }
}

void VideoController::handleSortOption(MainWindow::SortOptions option, bool ascending)
{
    if (!m_model)
        return;

    if (option == MainWindow::SortOptions::Size)
        m_model->sortVideosWithinGroupsBySize(ascending);
    else if (option == MainWindow::SortOptions::CreatedAt)
        m_model->sortVideosWithinGroupsByCreatedAt(ascending);
}

void VideoController::handleSortGroupsOption(MainWindow::SortOptions option, bool ascending)
{
    if (!m_model)
        return;

    if (option == MainWindow::SortOptions::Size)
        m_model->sortGroupsBySize(ascending);
    else if (option == MainWindow::SortOptions::CreatedAt)
        m_model->sortGroupsByCreatedAt(ascending);
}

void VideoController::handleDeleteOption(MainWindow::DeleteOptions option)
{
    if (!m_model)
        return;

    if (option == MainWindow::DeleteOptions::List)
        m_model->deleteSelectedVideos();
    else if (option == MainWindow::DeleteOptions::ListDB)
        qDebug() << "[TODO] Delete from DB not yet implemented";
    else if (option == MainWindow::DeleteOptions::Disk)
        qDebug() << "[TODO] Delete from disk not yet implemented";
}

void VideoController::setModel(VideoModel* model)
{
    if (!model) {
        qWarning() << "Attempted to set nullptr model!";
        return;
    }
    m_model = model;
}
