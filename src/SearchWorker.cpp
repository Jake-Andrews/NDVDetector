#include "SearchWorker.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include "Thumbnail.h"

#include <QDebug>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <unordered_set>

static constexpr double kSkipPercent = 0.15;

SearchWorker::SearchWorker(DatabaseManager& db,
    SearchSettings cfg,
    QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_cfg(std::move(cfg))
{
}

// ─────────────────────────────────────────────────────────────
void SearchWorker::process()
{
    // Lift global log-level if the caller hasn’t done so already
    spdlog::set_level(spdlog::level::debug);

    try {
        spdlog::info("[worker] Starting search task");

        // ── 1. Enumerate video files ──────────────────────────
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
            spdlog::info("[worker] Found {} videos in '{}'", vids.size(), dir.path);
            allVideos.insert(allVideos.end(), vids.begin(), vids.end());
        }
        emit filesFound(static_cast<int>(allVideos.size()));

        // ── 2. Filter videos already known to the DB ───────────
        std::unordered_set<std::string> known;
        auto dbVideos = m_db.getAllVideos();
        known.reserve(dbVideos.size());
        for (auto const& dv : dbVideos)
            known.insert(dv.path);

        std::erase_if(allVideos, [&](VideoInfo const& v) { return known.contains(v.path); });
        spdlog::info("[worker] {} new videos to process", allVideos.size());

        // ── 3. Metadata, thumbnails & DB insertion ─────────────
        doExtractionAndDetection(allVideos);

        // ── 4. Duplicate detection & persistence ──────────────
        auto all = m_db.getAllVideos();
        auto hashes = m_db.getAllHashGroups();
        auto groups = findDuplicates(std::move(all), hashes, 4, 5);
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

// ─────────────────────────────────────────────────────────────
void SearchWorker::doExtractionAndDetection(std::vector<VideoInfo>& videos)
{
    int hashedCount = 0;
    int totalToHash = static_cast<int>(videos.size());

    // Step A – metadata / thumbnail / initial DB insert
    std::erase_if(videos, [&](VideoInfo& v) {
        if (!extract_info(v)) {
            spdlog::warn("[meta] Failed FFprobe extraction – skipping '{}'", v.path);
            return true;
        }

        if (auto opt = extract_color_thumbnail(v.path))
            v.thumbnail_path = opt->toStdString();
        else
            v.thumbnail_path = "./sneed.png"; // fallback logo

        if (auto id = m_db.insertVideo(v)) {
            v.id = *id;
            return false;
        }
        spdlog::error("[DB] Inserting metadata failed for '{}'", v.path);
        return true;
    });

    // Step B – pHash extraction & DB insertion
    for (auto const& v : videos) {
        auto phashes = extract_phashes_from_video(
            v.path,
            kSkipPercent,
            v.duration,
            m_cfg.hwBackend,
            100);

        if (phashes.empty()) {
            spdlog::warn("[hash] No hashes generated for '{}'", v.path);
        } else if (!m_db.insertAllHashes(v.id, phashes)) {
            spdlog::error("[DB] Failed to insert {} hashes for '{}'",
                phashes.size(), v.path);
        } else {
            spdlog::debug("[hash] Stored {} hashes for '{}'",
                phashes.size(), v.path);
        }

        ++hashedCount;
        emit hashingProgress(hashedCount, totalToHash);
    }
}
