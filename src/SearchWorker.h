#pragma once

#include "DatabaseManager.h"
#include "VideoInfo.h"
#include "SearchSettings.h"
#include <QObject>
#include <QString>
#include <vector>

class SearchWorker : public QObject {
    Q_OBJECT

public:
    explicit SearchWorker(DatabaseManager& db, SearchSettings cfg, QObject* parent = nullptr);
    void process();

signals:
    void filesFound(int count);
    void hashingProgress(int done, int total);
    void error(QString message);
    void finished(std::vector<std::vector<VideoInfo>> duplicates);

private:
    DatabaseManager& m_db;
    SearchSettings m_cfg; 

    void doExtractionAndDetection(std::vector<VideoInfo>& videos);
};

