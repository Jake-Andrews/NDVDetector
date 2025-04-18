#pragma once
#include <QObject>
#include <QSet>
#include <filesystem>
#include "DatabaseManager.h"
#include "VideoInfo.h"

class HardlinkWorker : public QObject {
    Q_OBJECT
public:
    HardlinkWorker(DatabaseManager& db,
                   std::vector<std::vector<VideoInfo>> groups,
                   QSet<int>                     selectedIds,
                   QObject* parent = nullptr);

signals:
    void progress(int current, int total);
    void finished(std::vector<std::vector<VideoInfo>> updatedGroups,
                  int linksCreated,
                  int errors);

public slots:
    void process();

private:
    bool atomicHardlink(std::filesystem::path const& src,
                        std::filesystem::path const& dst,
                        std::error_code&             ec);

    DatabaseManager&                         m_db;
    std::vector<std::vector<VideoInfo>>      m_groups;
    QSet<int>                                m_selectedIds;
};

