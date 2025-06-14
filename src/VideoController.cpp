// VideoController.cpp
#include "VideoController.h"
#include "HardlinkWorker.h"
#include "SearchSettings.h"
#include "SearchWorker.h"
#include "VideoModel.h"

#include <filesystem>

#include <QDebug>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>

VideoController::VideoController(DatabaseManager& db, QObject* parent)
    : QObject(parent)
    , m_db(db)
{
}

void VideoController::startSearch()
{
    if (m_cfg.directories.empty()) {
        emit errorOccurred("No directories specified. Please add at least one directory.");
        return;
    }

    auto* progressDialog = new QProgressDialog(
        "Searching for videos...",
        "Cancel",
        0,
        0, // indeterminate at first
        nullptr);
    progressDialog->setWindowTitle("Searching");
    progressDialog->setWindowModality(Qt::ApplicationModal);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->setLabelText("Searching for videos…");
    progressDialog->show();

    QThread* thread = new QThread(this); // parent is this, so it cleans up
    SearchWorker* worker = new SearchWorker(m_db, m_cfg);
    worker->moveToThread(thread);

    // Connect signals/slots
    connect(thread, &QThread::started, worker, &SearchWorker::process);

    connect(worker, &SearchWorker::searchProgress,
        progressDialog, [progressDialog](int found) {
            progressDialog->setLabelText(
                QString("Searching for videos… %1 found").arg(found));
        });

    connect(worker, &SearchWorker::metadataProgress,
        progressDialog, [progressDialog](int done, int total) {
            progressDialog->setRange(0, total);
            progressDialog->setValue(done);
            progressDialog->setLabelText(
                QString("Generating metadata/thumbnails… %1/%2")
                    .arg(done)
                    .arg(total));
        });

    connect(worker, &SearchWorker::hashProgress,
        progressDialog, [progressDialog](int done, int total) {
            progressDialog->setRange(0, total);
            progressDialog->setValue(done);
            progressDialog->setLabelText(
                QString("Generating hashes… %1/%2")
                    .arg(done)
                    .arg(total));
        });

    // On error
    connect(worker, &SearchWorker::error, progressDialog,
        [progressDialog](QString msg) {
            QMessageBox::critical(progressDialog, "Error", msg);
        });

    // On finished
    connect(worker, &SearchWorker::finished, this,
        [=, this](std::vector<std::vector<VideoInfo>> duplicates) {
            progressDialog->close();
            progressDialog->deleteLater();

            emit duplicateGroupsUpdated(std::move(duplicates));

            thread->quit();
        });

    // Once thread is finished, delete worker + thread
    connect(thread, &QThread::finished, worker, &SearchWorker::deleteLater);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    // If user clicks cancel button
    connect(progressDialog, &QProgressDialog::canceled, this, [=]() {
        // **should requestStop for the worker or forcibly kill the thread**
        thread->requestInterruption();
        progressDialog->setLabelText("Canceling...");
    });

    thread->start();
}

void VideoController::onAddDirectoryRequested(QString const& path)
{
    std::filesystem::path fsPath(path.toStdString());
    if (!std::filesystem::exists(fsPath) || !std::filesystem::is_directory(fsPath)) {
        emit errorOccurred(QString("Directory does not exist:\n%1").arg(path));
        return;
    }

    auto it = std::find_if(m_cfg.directories.begin(), m_cfg.directories.end(), [&](DirectoryEntry const& e) {
        return e.path == path.toStdString();
    });

    if (it == m_cfg.directories.end()) {
        m_cfg.directories.push_back({ path.toStdString(), true });
    }

    QStringList dirList;
    for (auto const& d : m_cfg.directories) {
        dirList << QString::fromStdString(d.path);
    }
    emit directoryListUpdated(dirList);
}

void VideoController::onRemoveSelectedDirectoriesRequested(QStringList const& dirs)
{
    for (auto const& d : dirs) {
        auto it = std::remove_if(m_cfg.directories.begin(), m_cfg.directories.end(), [&](DirectoryEntry const& e) {
            return e.path == d.toStdString();
        });
        m_cfg.directories.erase(it, m_cfg.directories.end());
    }

    QStringList dirList;
    for (auto const& d : m_cfg.directories) {
        dirList << QString::fromStdString(d.path);
    }
    emit directoryListUpdated(dirList);
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
}

void VideoController::handleSortGroupsOption(MainWindow::SortOptions option, bool ascending)
{
    if (!m_model)
        return;

    if (option == MainWindow::SortOptions::Size)
        m_model->sortGroupsBySize(ascending);
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
        // gather all selected videos
        auto selected = m_model->selectedVideos();
        // delete them from DB
        for (auto const& v : selected) {
            if (v.id > 0) {
                m_db.deleteVideo(v.id);
            }
        }
        // remove from model
        std::vector<int> videoIds;
        videoIds.reserve(selected.size());
        for (auto const& vid : selected) {
            videoIds.push_back(vid.id);
        }

        m_model->removeVideosFromModel(videoIds);
        break;
    }

    case MainWindow::DeleteOptions::Disk: {
        auto selected = m_model->selectedVideos();

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

void VideoController::handleHardlink()
{
    if (!m_model)
        return;

    // collect selected UI state
    auto selected = m_model->selectedVideos();
    if (selected.empty())
        return;

    QSet<int> selectedIds;
    for (auto const& v : selected)
        selectedIds.insert(v.id);

    // grouped view of all rows
    auto groups = m_model->toGroups();

    // progress dialog
    auto* dlg = new QProgressDialog("Hard‑linking videos...", "Cancel", 0,
        static_cast<int>(groups.size()), nullptr);
    dlg->setWindowModality(Qt::ApplicationModal);
    dlg->setAutoClose(false);
    dlg->show();

    // worker thread
    QThread* t = new QThread(this);
    auto* wk = new HardlinkWorker(m_db, std::move(groups), std::move(selectedIds));
    wk->moveToThread(t);

    connect(t, &QThread::started, wk, &HardlinkWorker::process);
    connect(wk, &HardlinkWorker::progress, dlg, &QProgressDialog::setValue);

    connect(wk, &HardlinkWorker::finished, this,
        [=, this](std::vector<std::vector<VideoInfo>> updatedGroups,
            int links, int errs) {
            dlg->close();
            dlg->deleteLater();

            //  UI update
            m_model->fromGroups(updatedGroups);

            QString msg = tr("Hard‑linking complete.\n%1 file(s) hard‑linked.\n%2 error(s).")
                              .arg(links)
                              .arg(errs);
            QMessageBox::information(nullptr, tr("Done"), msg);

            t->quit();
        });

    connect(t, &QThread::finished, wk, &HardlinkWorker::deleteLater);
    connect(t, &QThread::finished, t, &QThread::deleteLater);

    connect(dlg, &QProgressDialog::canceled, this, [=] {
        t->requestInterruption();
        dlg->setLabelText("Canceling…");
    });

    t->start();
}

void VideoController::loadDatabase(QString const& path)
{
    if (!m_db.open(path, /*createIfMissing*/ false)) {
        emit errorOccurred(tr("Unable to open database:\n%1").arg(path));
        return;
    }

    auto groups = m_db.loadDuplicateGroups();
    auto settings = m_db.loadSettings();
    m_cfg = settings; // keep controller copy

    if (m_model)
        m_model->setGroupedVideos(groups);

    emit searchSettingsLoaded(settings);
    emit databaseOpened(path);
}

void VideoController::createDatabase(QString const& path)
{
    if (!m_db.open(path, /*createIfMissing*/ true)) {
        emit errorOccurred(tr("Unable to create database:\n%1").arg(path));
        return;
    }

    if (m_model)
        m_model->setGroupedVideos({});

    emit databaseOpened(path);
}

void VideoController::setSearchSettings(SearchSettings const& cfg)
{
    m_cfg = cfg;
}
