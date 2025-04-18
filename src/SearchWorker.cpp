#include "SearchWorker.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include <QDebug>
#include <filesystem>
#include <unordered_set>

SearchWorker::SearchWorker(DatabaseManager& db, QStringList directories, QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_directories(std::move(directories))
{
}

static double const SKIP_PERCENT = 0.15;

void SearchWorker::process()
{
    try {
        std::vector<VideoInfo> allVideos;

        for (QString const& dir : m_directories) {
            std::filesystem::path path(dir.toStdString());

            if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) {
                emit error(QString("Directory not valid or accessible: %1").arg(dir));
                continue;
            }

            auto vids = getVideosFromPath(path, { ".mp4", ".webm", ".mkv" });
            allVideos.insert(allVideos.end(), vids.begin(), vids.end());
        }

        emit filesFound((int)allVideos.size());

        // Remove already processed
        auto dbVideos = m_db.getAllVideos();
        std::unordered_set<std::string> knownPaths;
        knownPaths.reserve(dbVideos.size());
        for (auto const& dv : dbVideos) {
            knownPaths.insert(dv.path);
        }

        std::erase_if(allVideos, [&](VideoInfo const& v) {
            return knownPaths.contains(v.path);
        });

        doExtractionAndDetection(allVideos);

        auto all = m_db.getAllVideos();
        auto hashes = m_db.getAllHashGroups();

        auto groups = findDuplicates(std::move(all), hashes, 4, 5);
        m_db.storeDuplicateGroups(groups);
        emit finished(std::move(groups));

    } catch (std::exception const& e) {
        emit error(QString::fromStdString(e.what()));
    } catch (...) {
        emit error("Unknown error occurred during search.");
    }
}

void SearchWorker::doExtractionAndDetection(std::vector<VideoInfo>& videos)
{
    int hashedCount = 0;
    int totalToHash = (int)videos.size();

    // Extract metadata, store in DB
    std::erase_if(videos, [&](VideoInfo& v) {
        if (!extract_info(v)) {
            qWarning() << "Failed to extract info for:" << QString::fromStdString(v.path);
            return true;
        }

        if (auto opt = extract_color_thumbnail(v.path)) {
            v.thumbnail_path = opt->toStdString();
        } else {
            v.thumbnail_path = "./sneed.png";
        }
        m_db.insertVideo(v);
        return false;
    });

    // screenshots + pHashes
    for (auto const& v : videos) {
        auto frames = decode_video_frames_as_cimg(v.path, SKIP_PERCENT, v.duration);
        if (frames.empty()) {
            hashedCount++;
            emit hashingProgress(hashedCount, totalToHash);
            continue;
        }

        auto hashes = generate_pHashes(frames);
        if (!hashes.empty()) {
            m_db.insertAllHashes(v.id, hashes);
        }
        hashedCount++;
        emit hashingProgress(hashedCount, totalToHash);
    }
}
