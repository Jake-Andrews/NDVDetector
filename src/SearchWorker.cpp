#include "SearchWorker.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include <QDebug>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <unordered_set>

SearchWorker::SearchWorker(DatabaseManager& db, SearchSettings cfg, QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_cfg(std::move(cfg))
{
}

static double const SKIP_PERCENT = 0.15;

void SearchWorker::process()
{
    try {
        std::vector<VideoInfo> allVideos;

        for (auto const& dir : m_cfg.directories) {
            std::filesystem::path path(dir.path);

            if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) {
                emit error(
                    tr("Directory not valid or accessible: %1")
                        .arg(QString::fromStdString(dir.path)));
                continue;
            }

            auto vids = getVideosFromPath(dir.path, m_cfg);
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
    int totalToHash = static_cast<int>(videos.size());

    std::erase_if(videos, [&](VideoInfo& v) {
        if (!extract_info(v)) {
            spdlog::warn("[Worker] Skipping video due to metadata extraction failure: {}", v.path);
            return true;
        }

        if (auto opt = extract_color_thumbnail(v.path)) {
            v.thumbnail_path = opt->toStdString();
        } else {
            spdlog::warn("[Worker] Thumbnail generation failed for: {}", v.path);
            v.thumbnail_path = "./sneed.png";
        }

        auto id = m_db.insertVideo(v);
        if (!id) {
            spdlog::error("[DB] Failed to insert video metadata into database for: {}", v.path);
            return true;
        }
        v.id = *id;

        return false;
    });

    for (auto const& v : videos) {
        auto frames = decode_video_frames_as_cimg(v.path, SKIP_PERCENT, v.duration, m_cfg.hwBackend, 100);
        if (frames.empty()) {
            spdlog::warn("[Worker] No frames decoded for: {}", v.path);
            hashedCount++;
            emit hashingProgress(hashedCount, totalToHash);
            continue;
        }

        auto hashes = generate_pHashes(frames);

        hashedCount++;
        emit hashingProgress(hashedCount, totalToHash);

        if (hashes.empty()) {
            spdlog::warn("[Worker] No hashes generated for: {}", v.path);
            continue;
        }

        if (!m_db.insertAllHashes(v.id, hashes)) {
            spdlog::error("[DB] Failed to insert hashes for: {}", v.path);
        }
    }
}
