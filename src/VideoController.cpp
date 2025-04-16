#include "VideoController.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include "SearchWorker.h"
#include "VideoModel.h"

#include <iostream>

#include <QDebug>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>

VideoController::VideoController(DatabaseManager& db, QObject* parent)
    : QObject(parent)
    , m_db(db)
{
}

void VideoController::startSearchAndDetection(QString rootPath)
{
    // 1) Create progress dialog
    auto* progressDialog = new QProgressDialog(
        "Searching for videos...",
        "Cancel",
        0,   // min
        100, // max (we’ll adjust dynamically)
        nullptr);
    progressDialog->setWindowTitle("Searching");
    progressDialog->setWindowModality(Qt::ApplicationModal);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->show();

    // 2) Create worker and thread
    QThread* thread = new QThread(this); // parent is this, so it cleans up
    SearchWorker* worker = new SearchWorker(m_db, rootPath);
    worker->moveToThread(thread);

    // 3) Connect signals/slots
    connect(thread, &QThread::started, worker, &SearchWorker::process);

    // Update UI with # files found
    connect(worker, &SearchWorker::filesFound, progressDialog, [progressDialog](int count) {
        progressDialog->setLabelText(QString("Found %1 videos.\nScanning metadata...").arg(count));
        progressDialog->setRange(0, count); // update the max to "count"
    });

    // Update hashing progress
    connect(worker, &SearchWorker::hashingProgress, progressDialog,
        [progressDialog](int done, int total) {
            // setValue triggers the progress bar position
            progressDialog->setValue(done);
            progressDialog->setLabelText(
                QString("Hashing videos... %1/%2 done").arg(done).arg(total));
        });

    // If an error occurs
    connect(worker, &SearchWorker::error, progressDialog,
        [progressDialog](QString msg) {
            QMessageBox::critical(progressDialog, "Error", msg);
        });

    // On finished
    connect(worker, &SearchWorker::finished, this,
        [=, this](std::vector<std::vector<VideoInfo>> duplicates) {
            // 1) close the dialog
            progressDialog->close();
            progressDialog->deleteLater();

            // 2) send them to the view
            emit duplicateGroupsUpdated(std::move(duplicates));

            // 3) cleanup
            thread->quit();
        });

    // Once thread is finished, delete worker + thread
    connect(thread, &QThread::finished, worker, &SearchWorker::deleteLater);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    // 4) If user clicks cancel button
    connect(progressDialog, &QProgressDialog::canceled, this, [=]() {
        // you might want to requestStop for the worker or forcibly kill the thread.
        // For now, let's just do:
        thread->requestInterruption();
        progressDialog->setLabelText("Canceling...");
    });

    // 5) Start the thread
    thread->start();
}

void VideoController::runSearchAndDetection(QString rootPath)
{
    std::filesystem::path root(rootPath.toStdString());
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

        if (auto opt = extract_color_thumbnail(v.path)) {
            v.thumbnail_path = opt->toStdString();
        } else {
            v.thumbnail_path = "./sneed.png";
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

    switch (option) {
    case MainWindow::DeleteOptions::List: {
        m_model->deleteSelectedVideosFromList();
        break;
    }
    case MainWindow::DeleteOptions::ListDB: {
        // 1) gather all selected videos
        auto selected = m_model->selectedVideos();
        // 2) delete them from DB
        for (auto const& v : selected) {
            if (v.id > 0) {
                m_db.deleteVideo(v.id);
            }
        }
        // 3) remove from model
        std::vector<int> videoIds;
        videoIds.reserve(selected.size());
        for (auto const& vid : selected) {
            videoIds.push_back(vid.id);
        }
        m_model->removeVideosFromModel(videoIds);
        break;
    }
    case MainWindow::DeleteOptions::Disk: {
        // 1) gather all selected videos
        auto selected = m_model->selectedVideos();
        // 2) remove from disk and DB
        for (auto const& v : selected) {
            if (!v.path.empty()) {
                try {
                    std::filesystem::path filePath(v.path);
                    if (std::filesystem::exists(filePath)) {
                        std::filesystem::remove(filePath);
                    }
                } catch (std::exception const& e) {
                    qWarning() << "Failed to remove file:"
                               << QString::fromStdString(v.path)
                               << "Error:" << e.what();
                }
            }
            if (v.id > 0) {
                m_db.deleteVideo(v.id);
            }
        }
        // 3) remove from model
        std::vector<int> videoIds;
        videoIds.reserve(selected.size());
        for (auto const& vid : selected) {
            videoIds.push_back(vid.id);
        }
        m_model->removeVideosFromModel(videoIds);
        break;
    }
    default:
        qWarning() << "Unhandled DeleteOptions:" << (int)option;
        break;
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
