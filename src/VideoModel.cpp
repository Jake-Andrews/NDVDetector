#include "VideoModel.h"
#include <QColor>
#include <QDebug>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <algorithm>
#include <climits>
#include <numeric>

VideoModel::VideoModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void VideoModel::setGroupedVideos(std::vector<std::vector<VideoInfo>> const& groups)
{
    beginResetModel();
    m_rows.clear();
    m_groupBoundaries.clear();

    int groupIndex = 0;
    for (auto const& grp : groups) {
        // Insert a 'Separator' row to label this group
        RowEntry sep;
        sep.type = RowType::Separator;
        sep.label = QString("== GROUP %1 (%2 videos) ==").arg(++groupIndex).arg(grp.size());
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

    // It's a video row
    bool isSelected = row.selected;

    // For demonstration, let's show a check mark if selected, in col 0:
    if (role == Qt::CheckStateRole && index.column() == Col_Screenshot) {
        return isSelected ? Qt::Checked : Qt::Unchecked;
    }

    if (role == Qt::DisplayRole) {
        auto const& vid = row.video.value();
        switch (index.column()) {
        case Col_Path:
            return QString::fromStdString(vid.path);
        case Col_TechSpecs: {
            auto sizeKb = vid.size / 1024;
            auto br = vid.bit_rate / 1000;
            return QString("Size: %1 KB\nBitrate: %2 kb/s\nResolution: %3x%4\nFramerate: %5\nDuration: %6s")
                .arg(sizeKb)
                .arg(br)
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
        static QPixmap placeholder("sneed.png");
        if (!placeholder.isNull()) {
            QPixmap scaled = placeholder.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            return QIcon(scaled);
        }
    } else if (role == Qt::TextAlignmentRole && index.column() == Col_Screenshot) {
        // center screenshot
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
        return Qt::ItemIsEnabled;
    }

    // For video rows, let's allow user check/uncheck col 0
    if (index.column() == Col_Screenshot) {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
    }
    // normal columns are selectable only
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

bool VideoModel::setData(QModelIndex const& index, QVariant const& value, int role)
{
    if (!index.isValid() || index.row() >= (int)m_rows.size())
        return false;

    auto& row = m_rows[index.row()];
    if (row.type != RowType::Video)
        return false;

    // only handle col 0, checkStateRole
    if (role == Qt::CheckStateRole && index.column() == Col_Screenshot) {
        bool checked = (value.toInt() == Qt::Checked);
        row.selected = checked;

        // notify views
        emit dataChanged(index, index, { Qt::CheckStateRole });
        return true;
    }

    return false;
}

/*!
 * \brief selectAllExceptLargest
 *        Mark all video rows in each group as selected except for the largest
 */
void VideoModel::selectAllExceptLargest()
{
    // Clear all existing selections
    for (auto& row : m_rows) {
        if (row.type == RowType::Video) {
            row.selected = false;
        }
    }

    // We'll parse row by row
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
    // Clear all existing selections
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

void VideoModel::deleteSelectedVideos()
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

static int64_t safeStoll(std::string const& str)
{
    return std::stoll(str.empty() ? "0" : str);
}

void VideoModel::sortVideosWithinGroupsByCreatedAt(bool ascending)
{
    auto groups = toGroups();
    for (auto& g : groups) {
        std::sort(g.begin(), g.end(),
            [ascending](auto const& a, auto const& b) {
                auto atA = safeStoll(a.created_at);
                auto atB = safeStoll(b.created_at);
                return ascending ? atA < atB : atA > atB;
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

void VideoModel::sortGroupsByCreatedAt(bool ascending)
{
    auto groups = toGroups();
    std::sort(groups.begin(), groups.end(),
        [ascending](auto const& g1, auto const& g2) {
            if (g1.empty())
                return true;
            if (g2.empty())
                return false;

            auto valA = std::stoll(std::min_element(g1.begin(), g1.end(),
                [](auto const& a, auto const& b) {
                    return std::stoll(a.created_at.empty() ? "0" : a.created_at) < std::stoll(b.created_at.empty() ? "0" : b.created_at);
                })->created_at);

            auto valB = std::stoll(std::min_element(g2.begin(), g2.end(),
                [](auto const& a, auto const& b) {
                    return std::stoll(a.created_at.empty() ? "0" : a.created_at) < std::stoll(b.created_at.empty() ? "0" : b.created_at);
                })->created_at);

            return ascending ? valA < valB : valA > valB;
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
