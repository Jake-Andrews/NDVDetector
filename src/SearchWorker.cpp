#include "SearchWorker.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include "Thumbnail.h"

#include <QDebug>
#include <QRegularExpression>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <future>
#include <unordered_map>
#include <unordered_set>

SearchWorker::SearchWorker(DatabaseManager& db,
    SearchSettings cfg,
    QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_cfg(std::move(cfg))
{
}

void SearchWorker::process()
{
    // Lift global log-level if the caller hasn’t done so already
    spdlog::set_level(spdlog::level::debug);

    emit searchProgress(0);

    try {
        spdlog::info("[worker] Starting search task");

        // --- Enumerate video files ---
        std::vector<VideoInfo> allVideos;

        for (auto const& dir : m_cfg.directories) {
            std::filesystem::path p(dir.path);
            if (!std::filesystem::exists(p) || !std::filesystem::is_directory(p)) {
                auto msg = fmt::format("Directory not valid: {}", dir.path);
                spdlog::error("[worker] {}", msg);
                emit error(QString::fromStdString(msg));
                continue;
            }

            auto vids = getVideosFromPath(dir.path, m_cfg);
            for (auto const& v : vids) { // iterate *each* file
                allVideos.push_back(v);  // keep in master list
                emit searchProgress(static_cast<int>(allVideos.size()));
            }
        }

        // --- Filter videos already known to the DB ---
        std::unordered_set<std::string> known;
        auto dbVideos = m_db.getAllVideos();
        known.reserve(dbVideos.size());
        for (auto const& dv : dbVideos)
            known.insert(dv.path);

        std::erase_if(allVideos, [&](VideoInfo const& v) { return known.contains(v.path); });
        spdlog::info("[worker] {} new videos to process", allVideos.size());

        // --- Metadata, thumbnails & DB insertion ---
        doExtractionAndDetection(allVideos);

        // --- Duplicate detection & persistence ---
        auto all = m_db.getAllVideos();
        auto hashes = m_db.getAllHashGroups();

        auto groups = findDuplicates(std::move(all), hashes,
            m_cfg.hammingDistanceThreshold,
            m_cfg.usePercentThreshold,
            m_cfg.matchingThresholdPercent,
            m_cfg.matchingThresholdNumber);
        m_db.storeDuplicateGroups(groups);

        emit finished(std::move(groups));
        spdlog::info("[worker] Search task completed");

    } catch (std::exception const& e) {
        spdlog::error("[worker] Unhandled exception: {}", e.what());
        emit error(QString::fromStdString(e.what()));
    } catch (...) {
        spdlog::error("[worker] Unknown fatal error");
        emit error("Unknown error occurred during search.");
    }
}

void SearchWorker::doExtractionAndDetection(std::vector<VideoInfo>& videos)
{
    int metaDone = 0;
    int metaTotal = static_cast<int>(videos.size());
    emit metadataProgress(0, metaTotal);

    // --- metadata / thumbnail / initial DB insert ---
    spdlog::info("Thumbnail/FFprobe started");

    std::vector<VideoInfo> filtered;
    std::vector<std::future<std::pair<std::string, std::vector<QString>>>> thumbTasks;

    filtered.reserve(videos.size());
    thumbTasks.reserve(videos.size());

    for (auto& v : videos) {

        if (!extract_info(v)) {
            spdlog::warn("[meta] Failed FFprobe extraction – skipping '{}'", v.path);
            ++metaDone;
            emit metadataProgress(metaDone, metaTotal);
            continue;
        }

        ++metaDone;
        emit metadataProgress(metaDone, metaTotal);

        // copy for the worker thread
        VideoInfo vCopy = v;
        thumbTasks.emplace_back(std::async(std::launch::async,
            [vid = std::move(vCopy),
                nThumbs = m_cfg.thumbnailsPerVideo]() mutable
                -> std::pair<std::string, std::vector<QString>> {
                auto opt = extract_color_thumbnails(vid, nThumbs);
                return { vid.path, opt ? *opt : std::vector<QString> {} };
            }));
        filtered.push_back(std::move(v));
    }

    std::unordered_map<std::string, std::vector<QString>> thumbMap;
    for (auto& fut : thumbTasks) {
        // blocks only for unfinished tasks
        auto [path, qthumbs] = fut.get();
        thumbMap.emplace(std::move(path), std::move(qthumbs));
    }

    for (auto& v : filtered) {
        if (auto it = thumbMap.find(v.path); it != thumbMap.end()) {
            v.thumbnail_path.clear();
            v.thumbnail_path.reserve(it->second.size());
            for (auto const& q : it->second)
                v.thumbnail_path.push_back(q.toStdString());
        }
        // universal fallback
        if (v.thumbnail_path.empty())
            v.thumbnail_path.emplace_back("./sneed.png");

        // DB work kept serial
        if (auto id = m_db.insertVideo(v))
            v.id = *id;
        else
            spdlog::error("[DB] Inserting metadata failed for '{}'", v.path);
    }
    // preserve original contract
    videos.swap(filtered);

    spdlog::info("Thumbnail/FFprobe finished");
    spdlog::info("Hashing started");

    int hashedCount = 0;
    int totalToHash = static_cast<int>(videos.size());
    emit hashProgress(0, totalToHash);

    // --- pHash extraction & DB insertion ---
    for (auto const& v : videos) {
        try {
            spdlog::info("[hash] Processing '{}'", v.path);

            auto phashes = decode_and_hash(
                v.path,
                m_cfg.skipPercent,
                v.duration,
                m_cfg.maxFrames,
                {});

            if (phashes.empty()) {
                spdlog::warn("[hash] No hashes generated for '{}'", v.path);
            } else if (!m_db.insertAllHashes(v.id, phashes)) {
                spdlog::error("[DB] Failed to insert {} hashes for '{}'",
                    phashes.size(), v.path);
            } else {
                spdlog::info("[hash] Successfully stored {} hashes for '{}'",
                    phashes.size(), v.path);
            }
        } catch (std::exception const& ex) {
            spdlog::error("[worker] Exception while processing '{}': {}", v.path, ex.what());
        } catch (...) {
            spdlog::error("[worker] Unknown exception while processing '{}'", v.path);
        }

        ++hashedCount;
        emit hashProgress(hashedCount, totalToHash);
    }

    spdlog::info("Hashing finished: {} videos processed", hashedCount);
}
