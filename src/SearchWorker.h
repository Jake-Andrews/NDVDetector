#pragma once

#include "DatabaseManager.h"
#include "VideoInfo.h"
#include <QObject>
#include <QString>
#include <vector>

class SearchWorker : public QObject {
    Q_OBJECT
public:
    explicit SearchWorker(DatabaseManager& db,
        QString rootPath,
        QObject* parent = nullptr);

signals:
    void filesFound(int count);
    void hashingProgress(int done, int total);
    void finished(std::vector<std::vector<VideoInfo>> duplicates);
    void error(QString message);

public slots:
    void process();

private:
    DatabaseManager& m_db;
    QString m_rootPath;

    void doExtractionAndDetection(std::vector<VideoInfo>& videos);
};
