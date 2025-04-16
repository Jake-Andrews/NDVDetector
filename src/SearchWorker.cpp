#include "SearchWorker.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include <QDebug>
#include <filesystem>
#include <unordered_set>

SearchWorker::SearchWorker(DatabaseManager& db, QString rootPath, QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_rootPath(rootPath)
{
}

void SearchWorker::process()
{
    try {
        // 1) Find possible videos
        std::filesystem::path root(m_rootPath.toStdString());
        auto videos = getVideosFromPath(root, { ".mp4", ".webm" });
        emit filesFound((int)videos.size());

        // 2) Filter out previously processed videos
        auto dbVideos = m_db.getAllVideos();
        std::unordered_set<std::string> knownPaths;
        knownPaths.reserve(dbVideos.size());
        for (auto const& dv : dbVideos) {
            knownPaths.insert(dv.path);
        }

        std::erase_if(videos, [&](VideoInfo const& v) {
            return knownPaths.count(v.path) > 0;
        });

        // 3) Extract metadata, store in DB, and run pHash
        doExtractionAndDetection(videos);

        // 4) find duplicates from full db
        auto allVideos = m_db.getAllVideos();
        auto hashGroups = m_db.getAllHashGroups();

        auto duplicates = findDuplicates(std::move(allVideos), hashGroups, 4, 5);

        // 5) Emit finished signal
        emit finished(duplicates);
    } catch (std::exception const& e) {
        emit error(QString::fromStdString(e.what()));
    } catch (...) {
        emit error("Unknown error in search worker");
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
        auto frames = decode_video_frames_as_cimg(v.path);
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
