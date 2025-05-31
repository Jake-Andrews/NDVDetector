// SearchWorker.cpp
#include "SearchWorker.h"
#include "DuplicateDetector.h"
#include "FFProbeExtractor.h"
#include "FileSystemSearch.h"
#include "Thumbnail.h"
#include "VideoProcessorFactory.h"

#include <QDebug>
#include <QRegularExpression>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <future>
#include <unordered_map>
#include <unordered_set>

using enum HashMethod;

// helper ─ pick the right hash-config depending on method chosen
inline auto const& activeSlow(SearchSettings const& c) { return c.slowHash; }
inline auto const& activeFast(SearchSettings const& c) { return c.fastHash; }
inline bool isFast(SearchSettings const& c) { return c.method == Fast; }

SearchWorker::SearchWorker(DatabaseManager& db,
    SearchSettings cfg,
    QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_cfg(std::move(cfg))
    , m_proc(makeVideoProcessor(m_cfg))
{
}

void SearchWorker::process()
{
    spdlog::set_level(spdlog::level::debug);

    emit searchProgress(0);

    try {
        spdlog::info("[worker] Starting search task");

        // --- Gather video files and basic video information through syscalls  ---
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

        spdlog::info("[worker] found {} videos", allVideos.size());

        // --- Filter videos already known to the DB (equal paths) ---
        std::unordered_set<std::string> known;
        auto dbVideos = m_db.getAllVideos();
        known.reserve(dbVideos.size());
        for (auto const& dv : dbVideos)
            known.insert(dv.path);

        auto prev_size = allVideos.size();
        std::erase_if(allVideos, [&](VideoInfo const& v) { return known.contains(v.path); });
        spdlog::info("[worker] {} videos already found in the DB", prev_size - allVideos.size());
        spdlog::info("[worker] {} new videos to process", allVideos.size());

        // --- Generate metadata and thumbnails and DB insertion ---
        spdlog::info("[worker] Generating video metadata and thumbnails");
        generateMetadataAndThumbnails(allVideos);

        // --- pHash extraction & DB insertion ---
        decodeAndHashVideos(allVideos);

        // --- Get new and old hash groups and videos from the DB ---
        auto all = m_db.getAllVideos();
        auto hashes = m_db.getAllHashGroups();

        // --- Setup variables to be used depending on the hashing method chosen (fast or slow) ---
        bool const fast = isFast(m_cfg);
        int hamming = fast ? activeFast(m_cfg).hammingDistance
                           : activeSlow(m_cfg).hammingDistance;
        bool usePct = !fast && activeSlow(m_cfg).usePercentThreshold;
        double pctThr = usePct ? activeSlow(m_cfg).matchingThresholdPct : 0.0;
        std::uint64_t numThr = fast ? activeFast(m_cfg).matchingThreshold
                                    : activeSlow(m_cfg).matchingThresholdNum;

        // --- Compare video's pHashes to detect duplicates ---
        auto groups = findDuplicates(std::move(all), hashes,
            hamming,
            usePct,
            pctThr,
            numThr);
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

void SearchWorker::generateMetadataAndThumbnails(std::vector<VideoInfo>& videos)
{
    int metaDone = 0;
    int metaTotal = static_cast<int>(videos.size());
    emit metadataProgress(0, metaTotal);

    spdlog::info("Thumbnail/FFprobe started");

    std::vector<VideoInfo> filtered;
    std::vector<std::future<std::pair<std::string,
        std::vector<QString>>>>
        thumbTasks;

    filtered.reserve(videos.size());
    thumbTasks.reserve(videos.size());

    for (auto& v : videos) {

        if (!extract_info(v)) {
            spdlog::warn("[FFprobe] Failed extraction, skipping '{}'", v.path);
            ++metaDone;
            emit metadataProgress(metaDone, metaTotal);
            continue;
        }

        VideoInfo vCopy = v; // copy for async task
        thumbTasks.emplace_back(std::async(std::launch::async,
            [vid = std::move(vCopy), nThumbs = m_cfg.thumbnailsPerVideo]() mutable -> std::pair<std::string, std::vector<QString>> {
                auto opt = extract_color_thumbnails(vid, nThumbs);
                return { vid.path, opt ? *opt : std::vector<QString> {} };
            }));
        filtered.push_back(std::move(v));

        ++metaDone;
        emit metadataProgress(metaDone, metaTotal);
    }

    std::unordered_map<std::string, std::vector<QString>> thumbMap;
    for (auto& fut : thumbTasks) {
        auto [path, qthumbs] = fut.get(); // blocks only if needed
        thumbMap.emplace(std::move(path), std::move(qthumbs));
    }

    for (auto& v : filtered) {
        if (auto it = thumbMap.find(v.path); it != thumbMap.end()) {
            v.thumbnail_path.clear();
            v.thumbnail_path.reserve(it->second.size());
            for (auto const& q : it->second)
                v.thumbnail_path.push_back(q.toStdString());
        }
        if (v.thumbnail_path.empty())
            v.thumbnail_path.emplace_back("./sneed.png");

        if (auto id = m_db.insertVideo(v))
            v.id = *id;
        else
            spdlog::error("[DB] Inserting metadata failed for '{}'", v.path);
    }
    videos.swap(filtered);
    spdlog::info("Thumbnail/FFprobe finished");
}

void SearchWorker::decodeAndHashVideos(std::vector<VideoInfo>& videos)
{
    spdlog::info("Hashing started");

    int hashedCount = 0;
    int totalToHash = static_cast<int>(videos.size());
    emit hashProgress(0, totalToHash);

    for (auto const& v : videos) {
        try {
            spdlog::info("[hash] Processing '{}'", v.path);

            auto phashes = m_proc->decodeAndHash(v, m_cfg, {});

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
            spdlog::error("[worker] Exception while processing '{}': {}",
                v.path, ex.what());
        } catch (...) {
            spdlog::error("[worker] Unknown exception while processing '{}'",
                v.path);
        }

        ++hashedCount;
        emit hashProgress(hashedCount, totalToHash);
    }

    spdlog::info("Hashing finished: {} videos processed", hashedCount);
}
