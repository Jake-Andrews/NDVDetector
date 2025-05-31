#pragma once

#include "DatabaseManager.h"
#include "VideoInfo.h"
#include "SearchSettings.h"
#include <memory>
#include "IVideoProcessor.h"
#include <QObject>
#include <QString>
#include <vector>

class SearchWorker : public QObject {
    Q_OBJECT

public:
    explicit SearchWorker(DatabaseManager& db, SearchSettings cfg, QObject* parent = nullptr);
    void process();

signals:
    void searchProgress(int found);            
    void metadataProgress(int done, int total); 
    void hashProgress(int done, int total);    
    void error(QString message);
    void finished(std::vector<std::vector<VideoInfo>> duplicates);

private:
    DatabaseManager& m_db;
    SearchSettings   m_cfg;
    std::unique_ptr<IVideoProcessor> m_proc;   // strategy

    void doExtractionAndDetection(std::vector<VideoInfo>& videos);
    void generateMetadataAndThumbnails(std::vector<VideoInfo>& videos);
    void decodeAndHashVideos(std::vector<VideoInfo>& videos);
};

