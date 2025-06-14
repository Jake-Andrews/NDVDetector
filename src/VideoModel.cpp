// VideoModel.cpp
#include "VideoModel.h"
#include <QColor>
#include <QDebug>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QTableView>
#include <algorithm>
#include <climits>
#include <numeric>
#include <qnamespace.h>

VideoModel::VideoModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void VideoModel::setGroupedVideos(std::vector<std::vector<VideoInfo>> const& groups)
{
    // Filter out any groups that have fewer than 2 videos.
    std::vector<std::vector<VideoInfo>> filteredGroups;
    filteredGroups.reserve(groups.size());
    for (auto const& g : groups) {
        if (g.size() >= 2) {
            filteredGroups.push_back(g);
        }
    }

    beginResetModel();
    m_rows.clear();
    m_groupBoundaries.clear();

    int groupIndex = 0;
    for (auto const& grp : filteredGroups) {

        RowEntry sep;
        sep.type = RowType::Separator;

        int64_t totalSize = 0;
        for (auto const& v : grp) {
            totalSize += v.size;
        }
        double sizeGB = static_cast<double>(totalSize) / (1024.0 * 1024.0 * 1024.0);

        sep.label = QString("Group %1 (%2 Duplicates - Size: %3 GB)")
                        .arg(++groupIndex)
                        .arg(grp.size())
                        .arg(sizeGB, 0, 'f', 2);

        m_groupBoundaries.push_back(static_cast<int>(m_rows.size()));
        m_rows.push_back(std::move(sep));

        // Add each video row
        for (auto const& v : grp) {
            RowEntry row;
            row.type = RowType::Video;
            row.video = v;
            row.selected = false;
            m_rows.push_back(std::move(row));
        }
    }
    endResetModel();
}

int VideoModel::rowCount(QModelIndex const& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
}

int VideoModel::columnCount(QModelIndex const& parent) const
{
    if (parent.isValid())
        return 0;
    return Col_Count;
}

QVariant VideoModel::data(QModelIndex const& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};

    auto const& row = m_rows[index.row()];
    if (row.type == RowType::Separator) {
        if (role == Qt::DisplayRole)
            return row.label;
        if (role == Qt::TextAlignmentRole)
            return Qt::AlignCenter;
        if (role == Qt::BackgroundRole)
            return QColor(Qt::lightGray);
        if (role == Qt::FontRole) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return {};
    }

    bool isSelected = row.selected;

    if (role == Qt::CheckStateRole && index.column() == Col_Screenshot) {
        return isSelected ? Qt::Checked : Qt::Unchecked;
    }

    if (role == Qt::DisplayRole) {
        auto const& vid = row.video.value();
        switch (index.column()) {
        case Col_Path:
            return QString::fromStdString(vid.path);
        case Col_TechSpecs: {
            double sizeGB = static_cast<double>(vid.size) / (1024.0 * 1024.0 * 1024.0);
            double bitrateMbps = static_cast<double>(vid.bit_rate) / 1'000'000.0;

            return QString("Size: %1 GB\nBitrate: %2 Mbps\nResolution: %3x%4\nFramerate: %5\nDuration: %6s")
                .arg(sizeGB, 0, 'f', 2)
                .arg(bitrateMbps, 0, 'f', 2)
                .arg(vid.width)
                .arg(vid.height)
                .arg(vid.avg_frame_rate, 0, 'f', 2)
                .arg(vid.duration);
        }
        case Col_Codecs: {
            auto const& vc = vid.video_codec;
            auto const& ac = vid.audio_codec;
            return QString("Video: %1\nAudio: %2")
                .arg(QString::fromStdString(vc))
                .arg(QString::fromStdString(ac));
        }
        case Col_Links:
            return vid.num_hard_links;
        }
    } else if (role == Qt::DecorationRole && index.column() == Col_Screenshot) {
        auto const& vid = row.video.value();

        if (vid.thumbnail_path.empty())
            return QIcon("./placeholder.png");

        auto* view = qobject_cast<QTableView const*>(parent());
        int kCell = view ? view->iconSize().height() : 128;
        int nThumbs = std::min<std::size_t>(vid.thumbnail_path.size(),
            static_cast<size_t>(m_thumbnailsPerVideo));

        // helper – load & scale a pixmap so it fits into (maxW × maxH)
        auto loadScaled = [](QString const& fp, int maxW, int maxH) {
            QPixmap p(fp);
            if (p.isNull())
                return p;
            return p.scaled(maxW, maxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        };

        // --- single image → as before ---
        if (nThumbs == 1) {
            QPixmap p = loadScaled(QString::fromStdString(vid.thumbnail_path.front()), kCell, kCell);
            return p.isNull() ? QIcon("./placeholder.png") : QIcon(p);
        }

        // --- 2-4 images → side-by-side in one row with gap ---
        constexpr int gap = 4; // px between thumbs
        int canvasW = nThumbs * kCell + (nThumbs - 1) * gap;
        QPixmap canvas(canvasW, kCell);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);

        int x = 0;
        for (int i = 0; i < nThumbs; ++i) {
            QPixmap thumb = loadScaled(
                QString::fromStdString(vid.thumbnail_path[i]), kCell, kCell);
            if (!thumb.isNull())
                painter.drawPixmap(x, (kCell - thumb.height()) / 2, thumb);

            x += kCell + gap;
        }
        painter.end();
        return QIcon(canvas);
    }

    else if (role == Qt::TextAlignmentRole && index.column() == Col_Screenshot) {
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
    return {};
}

Qt::ItemFlags VideoModel::flags(QModelIndex const& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    auto const& row = m_rows[index.row()];
    if (row.type == RowType::Separator) {
        // Make separator rows enabled but not selectable or activatable
        return Qt::ItemIsEnabled;
    }

    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;

    if (index.column() == Col_Screenshot) {
        flags |= Qt::ItemIsUserCheckable;
    }

    return flags;
}

bool VideoModel::setData(QModelIndex const& index, QVariant const& value, int role)
{
    if (!index.isValid() || index.row() >= (int)m_rows.size())
        return false;

    auto& row = m_rows[index.row()];
    if (row.type != RowType::Video)
        return false;

    if (role == Qt::CheckStateRole && index.column() == Col_Screenshot) {
        bool checked = (value.toInt() == Qt::Checked);
        row.selected = checked;

        emit dataChanged(index, index, { Qt::CheckStateRole });
        return true;
    }

    return false;
}

void VideoModel::selectRow(int row)
{
    if (row < 0 || row >= (int)m_rows.size())
        return;

    auto& entry = m_rows[row];
    if (entry.type != RowType::Video)
        return;

    if (!entry.selected) {
        entry.selected = true;
        QModelIndex idx = createIndex(row, Col_Screenshot);
        emit dataChanged(idx, idx, { Qt::CheckStateRole });
    }
}

void VideoModel::selectAllExceptLargest()
{
    // Clear all existing selections
    for (auto& row : m_rows) {
        if (row.type == RowType::Video) {
            row.selected = false;
        }
    }

    // Parse row by row
    int currentGroupStart = -1;
    int currentGroupEnd = -1;

    for (int row = 0; row < (int)m_rows.size(); ++row) {
        if (m_rows[row].type == RowType::Separator) {
            // finalize previous group
            if (currentGroupStart != -1 && currentGroupEnd != -1) {
                markAllExceptLargestInRange(currentGroupStart, currentGroupEnd);
            }
            // start new group
            currentGroupStart = -1;
            currentGroupEnd = -1;
            continue;
        }
        // it's a Video
        if (currentGroupStart == -1) {
            currentGroupStart = row;
        }
        currentGroupEnd = row;
    }
    // finalize last group
    if (currentGroupStart != -1 && currentGroupEnd != -1) {
        markAllExceptLargestInRange(currentGroupStart, currentGroupEnd);
    }

    // notify the UI that check states changed
    QModelIndex tl = createIndex(0, 0);
    QModelIndex br = createIndex(rowCount() - 1, 0);
    emit dataChanged(tl, br, { Qt::CheckStateRole });
}

void VideoModel::selectAllExceptSmallest()
{
    for (auto& row : m_rows) {
        if (row.type == RowType::Video) {
            row.selected = false;
        }
    }

    int currentGroupStart = -1;
    int currentGroupEnd = -1;

    for (int row = 0; row < (int)m_rows.size(); ++row) {
        if (m_rows[row].type == RowType::Separator) {
            if (currentGroupStart != -1 && currentGroupEnd != -1) {
                markAllExceptSmallestInRange(currentGroupStart, currentGroupEnd);
            }
            currentGroupStart = -1;
            currentGroupEnd = -1;
            continue;
        }
        if (currentGroupStart == -1) {
            currentGroupStart = row;
        }
        currentGroupEnd = row;
    }
    if (currentGroupStart != -1 && currentGroupEnd != -1) {
        markAllExceptSmallestInRange(currentGroupStart, currentGroupEnd);
    }

    QModelIndex tl = createIndex(0, 0);
    QModelIndex br = createIndex(rowCount() - 1, 0);
    emit dataChanged(tl, br, { Qt::CheckStateRole });
}

void VideoModel::markAllExceptLargestInRange(int startRow, int endRow)
{
    if (startRow > endRow)
        return;
    int largestRow = -1;
    int64_t largestSize = -1;

    // find largest in [startRow..endRow]
    for (int r = startRow; r <= endRow; ++r) {
        auto& row = m_rows[r];
        if (row.type != RowType::Video)
            continue;
        auto const& vid = row.video.value();
        if (vid.size > largestSize) {
            largestSize = vid.size;
            largestRow = r;
        }
    }

    // Mark all except largest as selected
    for (int r = startRow; r <= endRow; ++r) {
        auto& row = m_rows[r];
        if (row.type != RowType::Video)
            continue;
        if (r != largestRow) {
            row.selected = true;
        }
    }
}

void VideoModel::markAllExceptSmallestInRange(int startRow, int endRow)
{
    if (startRow > endRow)
        return;
    int smallestRow = -1;
    int64_t smallestSize = LLONG_MAX;

    // find smallest in [startRow..endRow]
    for (int r = startRow; r <= endRow; ++r) {
        auto& row = m_rows[r];
        if (row.type != RowType::Video)
            continue;
        auto const& vid = row.video.value();
        if (vid.size < smallestSize) {
            smallestSize = vid.size;
            smallestRow = r;
        }
    }

    // Mark all except smallest as selected
    for (int r = startRow; r <= endRow; ++r) {
        auto& row = m_rows[r];
        if (row.type != RowType::Video)
            continue;
        if (r != smallestRow) {
            row.selected = true;
        }
    }
}

void VideoModel::deleteSelectedVideosFromList()
{
    beginResetModel();
    // remove any row that has selected = true, as long as it's a video
    m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(),
                     [&](RowEntry const& re) {
                         return (re.type == RowType::Video && re.selected);
                     }),
        m_rows.end());
    endResetModel();
}

void VideoModel::sortVideosWithinGroupsBySize(bool ascending)
{
    // Flatten each group, sort within group by size, rebuild
    auto groups = toGroups();

    for (auto& g : groups) {
        std::sort(g.begin(), g.end(),
            [ascending](VideoInfo const& a, VideoInfo const& b) {
                return ascending ? a.size < b.size : a.size > b.size;
            });
    }
    setGroupedVideos(groups);
}

void VideoModel::sortGroupsBySize(bool ascending)
{
    auto groups = toGroups();
    std::sort(groups.begin(), groups.end(),
        [ascending](auto const& g1, auto const& g2) {
            auto sum1 = std::accumulate(g1.begin(), g1.end(), int64_t { 0 },
                [](int64_t s, auto const& v) { return s + v.size; });
            auto sum2 = std::accumulate(g2.begin(), g2.end(), int64_t { 0 },
                [](int64_t s, auto const& v) { return s + v.size; });
            return ascending ? sum1 < sum2 : sum1 > sum2;
        });
    setGroupedVideos(groups);
}

RowEntry const& VideoModel::rowEntry(int row) const
{
    return m_rows.at(row);
}

// helper to flatten the model's row structure into a vector-of-groups
std::vector<std::vector<VideoInfo>> VideoModel::toGroups() const
{
    std::vector<std::vector<VideoInfo>> result;
    int currentGroupStart = -1;

    for (int row = 0; row < (int)m_rows.size(); ++row) {
        if (m_rows[row].type == RowType::Separator) {
            // start new group
            result.emplace_back();
            currentGroupStart = (int)result.size() - 1;

        } else if (m_rows[row].type == RowType::Video) {

            if (currentGroupStart < 0) {
                // if there's no separator yet, we create one
                result.emplace_back();
                currentGroupStart = (int)result.size() - 1;
            }

            auto const& v = m_rows[row].video.value();
            result[currentGroupStart].push_back(v);
        }
    }
    return result;
}

void VideoModel::fromGroups(std::vector<std::vector<VideoInfo>> const& groups)
{
    setGroupedVideos(groups);
}

std::vector<VideoInfo> VideoModel::selectedVideos() const
{
    std::vector<VideoInfo> vids;
    for (auto const& row : m_rows) {
        if (row.type == RowType::Video && row.selected) {
            vids.push_back(row.video.value());
        }
    }

    return vids;
}

void VideoModel::removeVideosFromModel(std::vector<int> const& videoIds)
{
    beginResetModel();
    m_rows.erase(
        std::remove_if(m_rows.begin(), m_rows.end(), [&](RowEntry const& re) {
            if (re.type != RowType::Video)
                return false;
            auto const& vid = re.video.value();
            // Erase it if it matches any ID in videoIds
            return (std::find(videoIds.begin(), videoIds.end(), vid.id) != videoIds.end());
        }),
        m_rows.end());
    endResetModel();
}
QSize VideoModel::span(QModelIndex const& index) const
{
    if (!index.isValid())
        return {};

    auto const& row = m_rows[index.row()];

    // Make the *first* cell of a separator row occupy the whole row.
    if (row.type == RowType::Separator) {
        if (index.column() == 0)
            return QSize(Col_Count, 1); // span all columns, one row
        else
            return QSize(1, 1); // inside the span → normal size
    }
    return QSize(1, 1); // normal cells
}

void VideoModel::updateVideoInfo(VideoInfo const& updated)
{
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        auto& row = m_rows[i];
        if (row.type != RowType::Video)
            continue;

        if (row.video->id == updated.id) {
            row.video = updated;

            for (int col = 0; col < Col_Count; ++col) {
                QVector<int> roles = { Qt::DisplayRole, Qt::CheckStateRole };
                if (col == Col_Screenshot)
                    // force thumbnail to be refreshed
                    roles.append(Qt::DecorationRole);
                emit dataChanged(index(i, col), index(i, col), roles);
            }

            break;
        }
    }
}

void VideoModel::updateVideosBulk(std::vector<VideoInfo> const& vids)
{
    for (auto const& v : vids)
        updateVideoInfo(v);
}

void VideoModel::setThumbnailsPerVideo(int n)
{
    n = std::clamp(n, 1, 4);
    if (m_thumbnailsPerVideo == n)
        return;
    m_thumbnailsPerVideo = n;

    // --- repaint all thumbnail cells ---
    for (int r = 0; r < rowCount(); ++r)
        if (m_rows[r].type == RowType::Video)
            emit dataChanged(index(r, Col_Screenshot),
                index(r, Col_Screenshot),
                { Qt::DecorationRole });
}
