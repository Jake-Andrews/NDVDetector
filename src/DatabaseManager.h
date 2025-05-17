#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sqlite3.h>
#include <QString>
#include "SearchSettings.h"
#include "Hash.h"
#include "VideoInfo.h"
#include "CodecTestWorker.h"      // for TestItem

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

    std::vector<TestItem> loadHardwareFilters() const;
    void                  upsertHardwareFilter(const TestItem& t);      // insert or update
    void                  updateHardwareFilterResult(const QString& path,
                                                     bool hwOk, bool swOk);
 
     bool open(QString const& file, bool createIfMissing);
 
 private:
     sqlite3* m_db = nullptr;
 
     void initDatabase();
     void execStatement(std::string const& sql);
     void populateHardwareFiltersIfEmpty();
 };

