#pragma once

#include <QObject>
#include <vector>
#include <filesystem>

#include "MainWindow.h"
#include "VideoInfo.h"
#include "DatabaseManager.h"

class VideoController : public QObject
{
    Q_OBJECT

public:
    explicit VideoController(DatabaseManager& db, QObject* parent = nullptr);

    void runSearchAndDetection();

    void handleSelectOption(const QString& option);
    void handleSortOption(const QString& option);
    void handleSortGroupsOption(const QString& option);
    void handleDeleteOption(const QString& option);
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

