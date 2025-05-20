#pragma once

#include "SearchSettings.h"
#include "Hash.h"
#include "VideoInfo.h"

#include <sqlite3.h>
#include <QString>

#include <string>
#include <vector>
#include <cstdint>

 class DatabaseManager {
 public:
     explicit DatabaseManager(std::string const& dbPath);
     ~DatabaseManager();
 
     DatabaseManager(DatabaseManager const&) = delete;
     DatabaseManager& operator=(DatabaseManager const&) = delete;
 
    std::optional<int> insertVideo(VideoInfo& video);
    bool insertAllHashes(int video_id, std::vector<uint64_t> const& pHashes);
 
     std::vector<VideoInfo> getAllVideos() const;
     std::vector<HashGroup> getAllHashGroups() const;
 
     void deleteVideo(int videoId);
     void copyMetadataExceptPath(int targetId, int destinationId);
     void updateHardLinkCount(int videoId, int count);
 
     void beginTransaction();
     void commit();
     void rollback();                

     void updateVideoInfo(VideoInfo const& v);
     void storeDuplicateGroups(std::vector<std::vector<VideoInfo>> const& groups);
     std::vector<std::vector<VideoInfo>> loadDuplicateGroups() const;

    SearchSettings loadSettings() const; 
    void saveSettings(SearchSettings const&);
 
     bool open(QString const& file, bool createIfMissing);
 
 private:
     sqlite3* m_db = nullptr;
 
     void initDatabase();
     void execStatement(std::string const& sql);
 };

