#pragma once

#include <QObject>
#include <vector>

#include "MainWindow.h"
#include "VideoInfo.h"
#include "DatabaseManager.h"

class VideoController : public QObject
{
    Q_OBJECT

public:
    explicit VideoController(DatabaseManager& db, QObject* parent = nullptr);

    void runSearchAndDetection();

    void handleSelectOption(MainWindow::SelectOptions option);
    void handleSortOption(MainWindow::SortOptions option, bool ascending);
    void handleSortGroupsOption(MainWindow::SortOptions option, bool ascending);
    void handleDeleteOption(MainWindow::DeleteOptions option);
    void setModel(VideoModel* model);

signals:
    void duplicateGroupsUpdated(std::vector<std::vector<VideoInfo>> const& groups);

private:
    DatabaseManager& m_db;

    VideoModel* m_model = nullptr; 

    std::vector<std::vector<VideoInfo>> m_currentGroups;

    void deleteFromListOnly();
    void deleteFromListAndDb();
    void deleteFromDisk();

    void sortBySize();
    void sortByCreatedAt();

    void sortGroupsBySize();
};

