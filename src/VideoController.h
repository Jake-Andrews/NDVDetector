#pragma once
#include <vector>
#include <filesystem>
#include "MainWindow.h"
#include "VideoInfo.h"
#include "DatabaseManager.h"

class VideoController
{
public:
    VideoController(DatabaseManager& db);

    std::vector<VideoInfo> gatherVideos(std::filesystem::path const& root);
    void removeAlreadyProcessed(std::vector<VideoInfo>& videos, std::vector<VideoInfo> const& existing);
    bool fillMetadata(VideoInfo& v);
    void storeVideoInDb(VideoInfo& v);
    void generateScreenshotsAndHashes(std::vector<VideoInfo> const& videos);
    std::vector<std::vector<VideoInfo>> detectDuplicates();
    void runSearchAndDetection(MainWindow& ui);

private:
    DatabaseManager& m_db;
};

