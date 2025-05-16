#include "SearchWorker.h"
#include "DecodingFrames.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include "Thumbnail.h"

#include <QDebug>
#include <filesystem>
#include <functional>
#include <spdlog/spdlog.h>
#include <unordered_set>

static constexpr double kSkipPercent = 0.15;
static constexpr int kMaxFrames = 50;

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

        // 1. Enumerate video files
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

        // 2. Filter videos already known to the DB
        std::unordered_set<std::string> known;
        auto dbVideos = m_db.getAllVideos();
        known.reserve(dbVideos.size());
        for (auto const& dv : dbVideos)
            known.insert(dv.path);

        std::erase_if(allVideos, [&](VideoInfo const& v) { return known.contains(v.path); });
        spdlog::info("[worker] {} new videos to process", allVideos.size());

        // 3. Metadata, thumbnails & DB insertion
        doExtractionAndDetection(allVideos);

        // ── 4. Duplicate detection & persistence ──────────────
        auto all = m_db.getAllVideos();
        auto hashes = m_db.getAllHashGroups();
        auto groups = findDuplicates(std::move(all), hashes, 4, 3);
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
    // Initialize counters and flags
    int hashedCount = 0;
    int totalToHash = static_cast<int>(videos.size());

    // Step A – metadata / thumbnail / initial DB insert
    spdlog::info("Thumbnail/FFprobe started");

    std::erase_if(videos, [&](VideoInfo& v) {
        if (!extract_info(v)) {
            spdlog::warn("[meta] Failed FFprobe extraction – skipping '{}'", v.path);
            return true;
        }

        // Try to extract thumbnail
        try {
            if (auto opt = extract_color_thumbnail(v.path))
                v.thumbnail_path = opt->toStdString();
            else
                v.thumbnail_path = "./sneed.png"; // fallback logo
        } catch (std::exception const& ex) {
            spdlog::error("[thumbnail] Exception during thumbnail extraction: {}", ex.what());
            v.thumbnail_path = "./sneed.png"; // fallback logo
        }

        // Try to insert into database
        if (auto id = m_db.insertVideo(v)) {
            v.id = *id;
            return false;
        }

        spdlog::error("[DB] Inserting metadata failed for '{}'", v.path);
        return true;
    });

    spdlog::info("Thumbnail/FFprobe finished");
    spdlog::info("Hashing started");

    // Check the m_cfg.hwBackend, if AV_HWDEVICE_TYPE_NONE then no hardware
    // accel
    // If a hwBackend was chosen compare the video's codec, profile, and level
    // to the hardware filters that the user created. If no filters were setup,
    // then use the default filter for the chosen backend.

    // Step B – pHash extraction & DB insertion
    for (auto const& v : videos) {
        try {
            spdlog::info("[hash] Processing '{}'", v.path);

            // HW decoding is enabled unless the UI forced "CPU-only"
            bool allowHW = (m_cfg.hwBackend != AV_HWDEVICE_TYPE_NONE);

            // Create a capture for the progress callback to avoid capturing 'this' by reference
            auto progressCallback = [worker = this, currentHashedCount = hashedCount,
                                        totalVideos = totalToHash](int pct) {
                // Forward fine-grained 0-100 information to the GUI thread safely
                QMetaObject::invokeMethod(
                    worker,
                    [=] {
                        worker->emit hashingProgress(currentHashedCount * 100 + pct, totalVideos * 100);
                    },
                    Qt::QueuedConnection);
            };

            auto phashes = extract_phashes_from_video(
                v.path,
                kSkipPercent,
                v.duration,
                kMaxFrames,
                allowHW,
                progressCallback);

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

        // Update progress counter safely
        ++hashedCount;

        // Notify UI of progress (this is safe to call from any thread with Qt)
        QMetaObject::invokeMethod(
            this,
            [this, hashedCount, totalToHash] {
                emit hashingProgress(hashedCount * 100, totalToHash * 100);
            },
            Qt::QueuedConnection);
    }

    spdlog::info("Hashing finished: {} videos processed", hashedCount);
}
