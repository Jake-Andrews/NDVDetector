#include "HardlinkWorker.h"
#include "DatabaseManager.h"
#include <QDebug>
#include <chrono>

using namespace std::chrono_literals;

HardlinkWorker::HardlinkWorker(DatabaseManager& db,
    std::vector<std::vector<VideoInfo>> groups,
    QSet<int> selectedIds,
    QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_groups(std::move(groups))
    , m_selectedIds(std::move(selectedIds))
{
}

static void copyFieldsExceptPath(VideoInfo& dst, VideoInfo const& src)
{
    dst.modified_at = src.modified_at;
    dst.video_codec = src.video_codec;
    dst.audio_codec = src.audio_codec;
    dst.width = src.width;
    dst.height = src.height;
    dst.duration = src.duration;
    dst.size = src.size;
    dst.bit_rate = src.bit_rate;
    dst.inode = src.inode;
    dst.device = src.device;
    dst.sample_rate_avg = src.sample_rate_avg;
    dst.avg_frame_rate = src.avg_frame_rate;
    dst.thumbnail_path = src.thumbnail_path;
}

bool HardlinkWorker::atomicHardlink(std::filesystem::path const& src,
    std::filesystem::path const& dst,
    std::error_code& ec)
{
    using namespace std::filesystem;
    auto baseDir = dst.parent_path();
    auto tmpName = baseDir / ("govdupes_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp");

    create_hard_link(src, tmpName, ec);
    if (ec)
        return false;

    rename(tmpName, dst, ec); // atomic on POSIX
    if (ec) {
        std::error_code ec2;
        remove(tmpName, ec2); // cleanup
        return false;
    }
    return true;
}

void HardlinkWorker::process()
{
    int totalGroups = static_cast<int>(m_groups.size());
    int processed = 0;
    int linksMade = 0;
    int errors = 0;

    std::vector<VideoInfo*> dirty; // pointers we touched – later bulk DB update

    for (auto& group : m_groups) {

        // 1) gather selected in this group
        std::vector<VideoInfo*> sel;
        for (auto& v : group)
            if (m_selectedIds.contains(v.id))
                sel.push_back(&v);

        if (sel.size() < 2) {
            emit progress(++processed, totalGroups);
            continue;
        }

        auto* source = sel.front();
        auto srcInode = source->inode;
        auto srcDevice = source->device;

        std::vector<VideoInfo*> alreadyLinked { source };
        std::vector<VideoInfo*> newlyLinked;

        // 2) iterate other selections
        for (size_t i = 1; i < sel.size(); ++i) {
            auto* v = sel[i];

            // skip if already linked
            if (v->inode == srcInode && v->device == srcDevice) {
                alreadyLinked.push_back(v);
                continue;
            }

            std::error_code ec;
            if (atomicHardlink(source->path, v->path, ec)) {
                ++linksMade;
                newlyLinked.push_back(v);

                v->inode = srcInode;
                v->device = srcDevice;

                // delete thumbnails
                for (auto const& p : v->thumbnail_path)
                    std::filesystem::remove(std::filesystem::path(p), ec);
            } else {
                qWarning() << "Hard‑link failed:" << v->path.c_str()
                           << "->" << source->path.c_str()
                           << ec.message().c_str();
                ++errors;
            }
        }

        // Re‑scan the whole group: anyone with the same inode/device now
        //  belongs to the hard‑link set, regardless of selection.
        std::vector<VideoInfo*> sameInode;
        for (auto& row : group) {
            if (row.inode == srcInode && row.device == srcDevice)
                sameInode.push_back(&row);
        }

        // Every entry must show the final count, which is the set size.
        uint64_t finalCount = static_cast<uint64_t>(sameInode.size());
        for (auto* v : sameInode) {
            v->num_hard_links = finalCount;
            // make sure newly linked rows carry full metadata too
            if (v != source)
                copyFieldsExceptPath(*v, *source);
        }

        dirty.insert(dirty.end(), sameInode.begin(), sameInode.end());

        emit progress(++processed, totalGroups);
    }

    // single DB transaction
    m_db.beginTransaction();
    for (auto* v : dirty) {
        m_db.updateVideoInfo(*v);
    }
    m_db.commit();

    emit finished(std::move(m_groups), linksMade, errors);
}
