#pragma once

#include <QAbstractTableModel>
#include <vector>
#include <optional>
#include <QString>
#include "VideoInfo.h"

enum class RowType {
    Video,
    Separator
};

struct RowEntry {
    RowType type;
    std::optional<VideoInfo> video;
    QString label;
    bool selected = false;  
};

class VideoModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Columns {
        Col_Screenshot = 0,
        Col_Path,
        Col_TechSpecs,
        Col_Codecs,
        Col_Links,
        Col_Count
    };

    explicit VideoModel(QObject* parent = nullptr);

    void setGroupedVideos(std::vector<std::vector<VideoInfo>> const& groups);

    QSize span(const QModelIndex& index) const override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void selectAllExceptLargest();
    void selectAllExceptSmallest();

    void deleteSelectedVideosFromList();  
    void removeVideosFromModel(std::vector<int> const& videoIds);

    void sortVideosWithinGroupsBySize(bool ascending);
    void sortVideosWithinGroupsByCreatedAt(bool ascending);

    void sortGroupsBySize(bool ascending);
    void sortGroupsByCreatedAt(bool ascending);

    void selectRow(int row);

    std::vector<std::vector<VideoInfo>> toGroups() const;

    void updateVideoInfo(VideoInfo const& updated);

    void updateVideosBulk(std::vector<VideoInfo> const& vidss);

    const RowEntry& rowEntry(int row) const;
    std::vector<VideoInfo> selectedVideos() const;

    void fromGroups(std::vector<std::vector<VideoInfo>> const& groups);

private:
    std::vector<RowEntry> m_rows;

    // store the row index of each group’s separator row
    // used if you want to know where each group starts
    std::vector<int> m_groupBoundaries;

    int m_thumbnailsPerVideo {4};

    void markAllExceptLargestInRange(int startRow, int endRow);
    void markAllExceptSmallestInRange(int startRow, int endRow);


    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

public:
    void setThumbnailsPerVideo(int n);

};

