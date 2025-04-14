#pragma once

#include <QAbstractTableModel>
#include <vector>
#include "VideoInfo.h"

/*!
 * \brief The VideoModel class
 *        A QAbstractTableModel that holds a list of VideoInfo structs
 *        and displays them in columns for Screenshot, Path, Tech Specs, Codecs, #Links.
 */

enum class RowType {
    Video,
    Separator
};

struct RowEntry {
    RowType type;
    std::optional<VideoInfo> video;
    QString label; 
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


    // QAbstractItemModel interface overrides:
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    std::vector<RowEntry> m_rows;
    std::vector<int> m_groupBoundaries;
};

