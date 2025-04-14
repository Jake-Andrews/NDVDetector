#include "VideoModel.h"
#include <QDebug>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QVariant>
#include <qfont.h>

VideoModel::VideoModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int VideoModel::rowCount(QModelIndex const& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int VideoModel::columnCount(QModelIndex const& parent) const
{
    if (parent.isValid())
        return 0;
    return Col_Count;
}

void VideoModel::setGroupedVideos(std::vector<std::vector<VideoInfo>> const& groups)
{
    beginResetModel();
    m_rows.clear();

    int groupNum = 1;
    for (auto const& group : groups) {
        // Add group label row
        RowEntry sep;
        sep.type = RowType::Separator;
        sep.label = QString("== GROUP %1 (%2 videos) ==").arg(groupNum++).arg(group.size());
        m_rows.push_back(std::move(sep));

        for (auto const& v : group) {
            RowEntry row;
            row.type = RowType::Video;
            row.video = v;
            m_rows.push_back(std::move(row));
        }
    }

    endResetModel();
}

QVariant VideoModel::data(QModelIndex const& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};

    auto const& row = m_rows[index.row()];

    if (row.type == RowType::Separator) {
        if (index.column() == 0) {
            if (role == Qt::DisplayRole) {
                return row.label;
            }
            if (role == Qt::TextAlignmentRole) {
                return Qt::AlignCenter;
            }
            if (role == Qt::FontRole) {
                QFont bold;
                bold.setBold(true);
                return bold;
            }
            if (role == Qt::BackgroundRole) {
                return QColor(Qt::lightGray);
            }
        } else {
            // All other columns in separator row: leave blank
            if (role == Qt::BackgroundRole) {
                return QColor(Qt::lightGray);
            }
            return {};
        }
    }

    if (!row.video.has_value()) {
        return {};
    }
    auto const& video = row.video.value();

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Col_Path:
            return QString::fromStdString(video.path);
        case Col_TechSpecs:
            return QString("Size: %1 KB\nBitrate: %2 kb/s\nResolution: %3x%4\nFramerate: %5\nDuration: %6s")
                .arg(video.size / 1024)
                .arg(video.bit_rate / 1000)
                .arg(video.width)
                .arg(video.height)
                .arg(video.avg_frame_rate, 0, 'f', 2)
                .arg(video.duration);
        case Col_Codecs:
            return QString("Video: %1\nAudio: %2")
                .arg(QString::fromStdString(video.video_codec))
                .arg(QString::fromStdString(video.audio_codec));
        case Col_Links:
            return video.num_hard_links;
        }
    } else if (role == Qt::DecorationRole && index.column() == Col_Screenshot) {
        static QPixmap placeholder("sneed.png");
        if (!placeholder.isNull()) {
            QPixmap scaled = placeholder.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            return QIcon(scaled);
        }
    } else if (role == Qt::TextAlignmentRole) {
        if (index.column() == Col_Screenshot)
            return Qt::AlignCenter;
    }

    return {};
}

QVariant VideoModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case Col_Screenshot:
            return "Screenshot";
        case Col_Path:
            return "Path";
        case Col_TechSpecs:
            return "Tech Specs";
        case Col_Codecs:
            return "Codecs";
        case Col_Links:
            return "#Links";
        }
    }
    return QVariant();
}
