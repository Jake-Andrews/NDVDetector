#pragma once

#include <QObject>
#include <QString>
#include <QFutureWatcher>
#include <QPointer>

#include <vector>
#include <atomic>

#include "MainWindow.h"
#include "VideoInfo.h"
#include "DatabaseManager.h"

class SearchProgressDialog; 
class VideoModel;

class VideoController : public QObject
{
    Q_OBJECT

public:
    explicit VideoController(DatabaseManager& db, QObject* parent = nullptr);

    void setModel(VideoModel* model);

public slots: 
    void startSearchAndDetection();

    void onAddDirectoryRequested(const QString& path);
    void onRemoveSelectedDirectoriesRequested(const QStringList& dirs);

    void handleSelectOption(MainWindow::SelectOptions option);
    void handleSortOption(MainWindow::SortOptions option, bool ascending);
    void handleSortGroupsOption(MainWindow::SortOptions option, bool ascending);
    void handleDeleteOption(MainWindow::DeleteOptions option);
    void handleHardlink();
    void loadDatabase(QString const& path);
    void createDatabase(QString const& path);


signals:
    void duplicateGroupsUpdated(std::vector<std::vector<VideoInfo>> const& groups);

    void directoryListUpdated(const QStringList& directories);

    void errorOccurred(const QString& message);

    void updateProgress(QString status, int current, int total);
    void searchCompleted();

    void databaseOpened(QString const& path);

private:
    DatabaseManager& m_db;
    VideoModel* m_model = nullptr;
    std::vector<std::vector<VideoInfo>> m_currentGroups;

    QStringList m_directories;

    QPointer<SearchProgressDialog> m_progressDialog;
    QFutureWatcher<void> m_searchWatcher;

    std::atomic<int> m_totalVideos { 0 };
    std::atomic<int> m_processedVideos { 0 };
    std::atomic<int> m_filesFound { 0 };

    void deleteFromListOnly();
    void deleteFromListAndDb();
    void deleteFromDisk();

    void sortBySize();
    void sortByCreatedAt();
    void sortGroupsBySize();
    void sortGroupsByCreatedAt();
};

