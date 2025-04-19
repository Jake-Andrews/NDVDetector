#include "DatabaseManager.h"
#include "Hash.h"
#include "SearchSettings.h"
#include "VideoInfo.h"

#include <memory>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <stdexcept>

namespace {

using SqliteStmtPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

SqliteStmtPtr prepareStatement(sqlite3* db, std::string const& sql)
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            "SQLite prepare failed for [" + sql + "]: " + sqlite3_errmsg(db));
    }
    return SqliteStmtPtr(stmt, &sqlite3_finalize);
}

void checkRc(int rc, sqlite3* db, std::string const& context)
{
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
        spdlog::error("{}: {}", context, sqlite3_errmsg(db));
        throw std::runtime_error(context + ": " + sqlite3_errmsg(db));
    }
}

} // namespace

DatabaseManager::DatabaseManager(std::string const& dbPath)
    : m_db(nullptr)
{
    int rc = sqlite3_open(dbPath.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(m_db);
        sqlite3_close(m_db);
        m_db = nullptr;
        throw std::runtime_error("Cannot open database [" + dbPath + "]: " + err);
    }

    try {
        execStatement("PRAGMA foreign_keys = ON;");
        initDatabase();
    } catch (std::exception const& ex) {
        spdlog::error("Failed to initialize database: {}", ex.what());
        throw;
    }
}

DatabaseManager::~DatabaseManager()
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void DatabaseManager::insertVideo(VideoInfo& video)
{
    static constexpr auto sql = R"(
        INSERT INTO video (
            path, created_at, modified_at,
            video_codec, audio_codec, width, height,
            duration, size, bit_rate, num_hard_links,
            inode, device, sample_rate_avg, avg_frame_rate, thumbnail_path
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);
    )";

    try {
        auto stmt = prepareStatement(m_db, sql);
        checkRc(sqlite3_bind_text(stmt.get(), 1, video.path.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind path");
        checkRc(sqlite3_bind_text(stmt.get(), 2, video.created_at.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind created_at");
        checkRc(sqlite3_bind_text(stmt.get(), 3, video.modified_at.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind modified_at");
        checkRc(sqlite3_bind_text(stmt.get(), 4, video.video_codec.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind video_codec");
        checkRc(sqlite3_bind_text(stmt.get(), 5, video.audio_codec.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind audio_codec");
        checkRc(sqlite3_bind_int(stmt.get(), 6, video.width), m_db, "bind width");
        checkRc(sqlite3_bind_int(stmt.get(), 7, video.height), m_db, "bind height");
        checkRc(sqlite3_bind_int(stmt.get(), 8, video.duration), m_db, "bind duration");
        checkRc(sqlite3_bind_int(stmt.get(), 9, video.size), m_db, "bind size");
        checkRc(sqlite3_bind_int(stmt.get(), 10, video.bit_rate), m_db, "bind bit_rate");
        checkRc(sqlite3_bind_int(stmt.get(), 11, video.num_hard_links), m_db, "bind num_hard_links");
        checkRc(sqlite3_bind_int64(stmt.get(), 12, static_cast<sqlite3_int64>(video.inode)), m_db, "bind inode");
        checkRc(sqlite3_bind_int64(stmt.get(), 13, static_cast<sqlite3_int64>(video.device)), m_db, "bind device");
        checkRc(sqlite3_bind_int(stmt.get(), 14, video.sample_rate_avg), m_db, "bind sample_rate_avg");
        checkRc(sqlite3_bind_double(stmt.get(), 15, video.avg_frame_rate), m_db, "bind avg_frame_rate");
        checkRc(sqlite3_bind_text(stmt.get(), 16, video.thumbnail_path.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind thumbnail_path");
        checkRc(sqlite3_step(stmt.get()), m_db, "execute insertVideo");

        video.id = static_cast<int>(sqlite3_last_insert_rowid(m_db));
    } catch (std::exception const& ex) {
        spdlog::error("insertVideo failed: {}", ex.what());
        throw;
    }
}

void DatabaseManager::insertAllHashes(int video_id, std::vector<uint64_t> const& pHashes)
{
    if (pHashes.empty())
        return;

    static constexpr auto sql = R"(
        INSERT INTO hash (video_id, hash_blob) VALUES (?,?);
    )";

    try {
        auto stmt = prepareStatement(m_db, sql);
        checkRc(sqlite3_bind_int(stmt.get(), 1, video_id), m_db, "bind video_id");
        checkRc(
            sqlite3_bind_blob(
                stmt.get(),
                2,
                reinterpret_cast<void const*>(pHashes.data()),
                static_cast<int>(pHashes.size() * sizeof(uint64_t)),
                SQLITE_TRANSIENT),
            m_db,
            "bind hash_blob");
        checkRc(sqlite3_step(stmt.get()), m_db, "execute insertAllHashes");
    } catch (std::exception const& ex) {
        spdlog::error("insertAllHashes failed: {}", ex.what());
        throw;
    }
}

std::vector<VideoInfo> DatabaseManager::getAllVideos() const
{
    static constexpr auto sql = R"(
        SELECT id, path, created_at, modified_at,
               video_codec, audio_codec, width, height,
               duration, size, bit_rate, num_hard_links,
               inode, device, sample_rate_avg, avg_frame_rate, thumbnail_path
        FROM video
        ORDER BY id ASC;
    )";

    std::vector<VideoInfo> results;
    try {
        auto stmt = prepareStatement(m_db, sql);
        while (true) {
            int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_ROW) {
                VideoInfo v;
                v.id = sqlite3_column_int(stmt.get(), 0);
                v.path = reinterpret_cast<char const*>(sqlite3_column_text(stmt.get(), 1));
                v.created_at = reinterpret_cast<char const*>(sqlite3_column_text(stmt.get(), 2));
                v.modified_at = reinterpret_cast<char const*>(sqlite3_column_text(stmt.get(), 3));
                v.video_codec = reinterpret_cast<char const*>(sqlite3_column_text(stmt.get(), 4));
                v.audio_codec = reinterpret_cast<char const*>(sqlite3_column_text(stmt.get(), 5));
                v.width = sqlite3_column_int(stmt.get(), 6);
                v.height = sqlite3_column_int(stmt.get(), 7);
                v.duration = sqlite3_column_int(stmt.get(), 8);
                v.size = sqlite3_column_int(stmt.get(), 9);
                v.bit_rate = sqlite3_column_int(stmt.get(), 10);
                v.num_hard_links = sqlite3_column_int(stmt.get(), 11);
                v.inode = static_cast<long>(sqlite3_column_int64(stmt.get(), 12));
                v.device = static_cast<long>(sqlite3_column_int64(stmt.get(), 13));
                v.sample_rate_avg = sqlite3_column_int(stmt.get(), 14);
                v.avg_frame_rate = sqlite3_column_double(stmt.get(), 15);
                char const* t = reinterpret_cast<char const*>(sqlite3_column_text(stmt.get(), 16));
                v.thumbnail_path = t ? t : "";
                results.push_back(std::move(v));
            } else if (rc == SQLITE_DONE) {
                break;
            } else {
                throw std::runtime_error("Error stepping getAllVideos: " + std::string(sqlite3_errmsg(m_db)));
            }
        }
    } catch (std::exception const& ex) {
        spdlog::error("getAllVideos failed: {}", ex.what());
        throw;
    }
    return results;
}

std::vector<HashGroup> DatabaseManager::getAllHashGroups() const
{
    static constexpr auto sql = "SELECT video_id, hash_blob FROM hash;";
    std::vector<HashGroup> results;
    try {
        auto stmt = prepareStatement(m_db, sql);
        while (true) {
            int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_ROW) {
                int vid = sqlite3_column_int(stmt.get(), 0);
                auto blobPtr = sqlite3_column_blob(stmt.get(), 1);
                int bytes = sqlite3_column_bytes(stmt.get(), 1);
                if (blobPtr && bytes > 0) {
                    size_t count = bytes / sizeof(uint64_t);
                    auto const* raw = static_cast<uint64_t const*>(blobPtr);
                    HashGroup grp;
                    grp.fk_hash_video = vid;
                    grp.hashes.assign(raw, raw + count);
                    results.push_back(std::move(grp));
                }
            } else if (rc == SQLITE_DONE) {
                break;
            } else {
                throw std::runtime_error("Error stepping getAllHashGroups: " + std::string(sqlite3_errmsg(m_db)));
            }
        }
    } catch (std::exception const& ex) {
        spdlog::error("getAllHashGroups failed: {}", ex.what());
        throw;
    }
    return results;
}

void DatabaseManager::deleteVideo(int videoId)
{
    static constexpr auto sql = "DELETE FROM video WHERE id = ?;";
    try {
        auto stmt = prepareStatement(m_db, sql);
        checkRc(sqlite3_bind_int(stmt.get(), 1, videoId), m_db, "bind deleteVideo id");
        checkRc(sqlite3_step(stmt.get()), m_db, "execute deleteVideo");
    } catch (std::exception const& ex) {
        spdlog::error("deleteVideo failed: {}", ex.what());
        throw;
    }
}

void DatabaseManager::copyMetadataExceptPath(int targetId, int destinationId)
{
    static constexpr auto sql = R"(
        UPDATE video SET
            created_at     = (SELECT created_at FROM video WHERE id = ?),
            modified_at    = (SELECT modified_at FROM video WHERE id = ?),
            video_codec    = (SELECT video_codec FROM video WHERE id = ?),
            audio_codec    = (SELECT audio_codec FROM video WHERE id = ?),
            width          = (SELECT width FROM video WHERE id = ?),
            height         = (SELECT height FROM video WHERE id = ?),
            duration       = (SELECT duration FROM video WHERE id = ?),
            size           = (SELECT size FROM video WHERE id = ?),
            bit_rate       = (SELECT bit_rate FROM video WHERE id = ?),
            num_hard_links = (SELECT num_hard_links FROM video WHERE id = ?),
            inode          = (SELECT inode FROM video WHERE id = ?),
            device         = (SELECT device FROM video WHERE id = ?),
            sample_rate_avg= (SELECT sample_rate_avg FROM video WHERE id = ?),
            avg_frame_rate = (SELECT avg_frame_rate FROM video WHERE id = ?),
            thumbnail_path = (SELECT thumbnail_path FROM video WHERE id = ?)
        WHERE id = ?;
    )";

    try {
        auto stmt = prepareStatement(m_db, sql);
        for (int i = 1; i <= 15; ++i) {
            checkRc(sqlite3_bind_int(stmt.get(), i, targetId), m_db, "bind metadata field");
        }
        checkRc(sqlite3_bind_int(stmt.get(), 16, destinationId), m_db, "bind destinationId");
        checkRc(sqlite3_step(stmt.get()), m_db, "execute copyMetadataExceptPath");
    } catch (std::exception const& ex) {
        spdlog::error("copyMetadataExceptPath failed: {}", ex.what());
        throw;
    }
}

void DatabaseManager::updateHardLinkCount(int videoId, int count)
{
    static constexpr auto sql = R"(
        UPDATE video SET num_hard_links = ? WHERE id = ?;
    )";
    try {
        auto stmt = prepareStatement(m_db, sql);
        checkRc(sqlite3_bind_int(stmt.get(), 1, count), m_db, "bind hard link count");
        checkRc(sqlite3_bind_int(stmt.get(), 2, videoId), m_db, "bind video id");
        checkRc(sqlite3_step(stmt.get()), m_db, "execute updateHardLinkCount");
    } catch (std::exception const& ex) {
        spdlog::error("updateHardLinkCount failed: {}", ex.what());
        throw;
    }
}

void DatabaseManager::updateVideoInfo(VideoInfo const& v)
{
    static constexpr auto sql = R"(
        UPDATE video SET
            path             = ?,
            created_at       = ?,
            modified_at      = ?,
            video_codec      = ?,
            audio_codec      = ?,
            width            = ?,
            height           = ?,
            duration         = ?,
            size             = ?,
            bit_rate         = ?,
            num_hard_links   = ?,
            inode            = ?,
            device           = ?,
            sample_rate_avg  = ?,
            avg_frame_rate   = ?,
            thumbnail_path   = ?
        WHERE id = ?;
    )";
    try {
        auto stmt = prepareStatement(m_db, sql);
        checkRc(sqlite3_bind_text(stmt.get(), 1, v.path.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind path");
        checkRc(sqlite3_bind_text(stmt.get(), 2, v.created_at.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind created_at");
        checkRc(sqlite3_bind_text(stmt.get(), 3, v.modified_at.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind modified_at");
        checkRc(sqlite3_bind_text(stmt.get(), 4, v.video_codec.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind video_codec");
        checkRc(sqlite3_bind_text(stmt.get(), 5, v.audio_codec.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind audio_codec");
        checkRc(sqlite3_bind_int(stmt.get(), 6, v.width), m_db, "bind width");
        checkRc(sqlite3_bind_int(stmt.get(), 7, v.height), m_db, "bind height");
        checkRc(sqlite3_bind_int(stmt.get(), 8, v.duration), m_db, "bind duration");
        checkRc(sqlite3_bind_int(stmt.get(), 9, v.size), m_db, "bind size");
        checkRc(sqlite3_bind_int(stmt.get(), 10, v.bit_rate), m_db, "bind bit_rate");
        checkRc(sqlite3_bind_int(stmt.get(), 11, v.num_hard_links), m_db, "bind num_hard_links");
        checkRc(sqlite3_bind_int64(stmt.get(), 12, static_cast<sqlite3_int64>(v.inode)), m_db, "bind inode");
        checkRc(sqlite3_bind_int64(stmt.get(), 13, static_cast<sqlite3_int64>(v.device)), m_db, "bind device");
        checkRc(sqlite3_bind_int(stmt.get(), 14, v.sample_rate_avg), m_db, "bind sample_rate_avg");
        checkRc(sqlite3_bind_double(stmt.get(), 15, v.avg_frame_rate), m_db, "bind avg_frame_rate");
        checkRc(sqlite3_bind_text(stmt.get(), 16, v.thumbnail_path.c_str(), -1, SQLITE_TRANSIENT), m_db, "bind thumbnail_path");
        checkRc(sqlite3_bind_int(stmt.get(), 17, v.id), m_db, "bind id");
        checkRc(sqlite3_step(stmt.get()), m_db, "execute updateVideoInfo");
    } catch (std::exception const& ex) {
        spdlog::error("updateVideoInfo failed: {}", ex.what());
        throw;
    }
}

void DatabaseManager::storeDuplicateGroups(std::vector<std::vector<VideoInfo>> const& groups)
{
    static constexpr auto insertGrp = "INSERT INTO dup_group DEFAULT VALUES;";
    static constexpr auto insertMap = "INSERT INTO dup_group_map (group_id, video_id) VALUES (?,?);";

    try {
        beginTransaction();
        execStatement("DELETE FROM dup_group;");

        auto stmtMap = prepareStatement(m_db, insertMap);
        for (auto const& g : groups) {
            execStatement(insertGrp);
            int grpId = static_cast<int>(sqlite3_last_insert_rowid(m_db));

            for (auto const& v : g) {
                checkRc(sqlite3_reset(stmtMap.get()), m_db, "reset dup_group_map stmt");
                checkRc(sqlite3_bind_int(stmtMap.get(), 1, grpId), m_db, "bind group_id");
                checkRc(sqlite3_bind_int(stmtMap.get(), 2, v.id), m_db, "bind video_id");
                checkRc(sqlite3_step(stmtMap.get()), m_db, "execute dup_group_map insert");
            }
        }
        commit();
    } catch (std::exception const& ex) {
        spdlog::error("storeDuplicateGroups failed: {}", ex.what());
        rollback();
        throw;
    }
}

std::vector<std::vector<VideoInfo>> DatabaseManager::loadDuplicateGroups() const
{
    auto allVideos = getAllVideos();
    std::unordered_map<int, VideoInfo> id2vid;
    for (auto& v : allVideos)
        id2vid[v.id] = v;

    std::vector<std::vector<VideoInfo>> groups;
    static constexpr auto sql = "SELECT group_id, video_id FROM dup_group_map ORDER BY group_id;";
    try {
        auto stmt = prepareStatement(m_db, sql);
        int currentGrp = -1;
        while (true) {
            int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_ROW) {
                int gId = sqlite3_column_int(stmt.get(), 0);
                int vId = sqlite3_column_int(stmt.get(), 1);
                if (gId != currentGrp) {
                    groups.emplace_back();
                    currentGrp = gId;
                }
                auto it = id2vid.find(vId);
                if (it != id2vid.end()) {
                    groups.back().push_back(it->second);
                }
            } else if (rc == SQLITE_DONE) {
                break;
            } else {
                throw std::runtime_error("Error stepping loadDuplicateGroups: " + std::string(sqlite3_errmsg(m_db)));
            }
        }
    } catch (std::exception const& ex) {
        spdlog::error("loadDuplicateGroups failed: {}", ex.what());
        throw;
    }
    return groups;
}

bool DatabaseManager::open(QString const& file, bool createIfMissing)
{
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }

    int flags = SQLITE_OPEN_READWRITE | (createIfMissing ? SQLITE_OPEN_CREATE : 0);
    sqlite3* newDb = nullptr;
    int rc = sqlite3_open_v2(file.toStdString().c_str(), &newDb, flags, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("open failed for {}: {}", file.toStdString(), sqlite3_errmsg(newDb));
        if (newDb)
            sqlite3_close(newDb);
        return false;
    }

    m_db = newDb;
    try {
        execStatement("PRAGMA foreign_keys = ON;");
        initDatabase();
    } catch (std::exception const& ex) {
        spdlog::error("initDatabase failed in open: {}", ex.what());
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    return true;
}

void DatabaseManager::initDatabase()
{
    static constexpr auto createVideoTableSQL = R"(
        CREATE TABLE IF NOT EXISTS video (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT NOT NULL,
            created_at DATETIME,
            modified_at DATETIME,
            video_codec TEXT,
            audio_codec TEXT,
            width INTEGER,
            height INTEGER,
            duration INTEGER NOT NULL,
            size INTEGER NOT NULL,
            bit_rate INTEGER,
            num_hard_links INTEGER,
            inode INTEGER,
            device INTEGER,
            sample_rate_avg INTEGER,
            avg_frame_rate REAL,
            thumbnail_path TEXT
        );
    )";
    static constexpr auto createHashTableSQL = R"(
        CREATE TABLE IF NOT EXISTS hash (
            video_id INTEGER PRIMARY KEY,
            hash_blob BLOB NOT NULL,
            FOREIGN KEY(video_id) REFERENCES video(id) ON DELETE CASCADE
        );
    )";
    static constexpr auto createDupGroupTable = R"(
        CREATE TABLE IF NOT EXISTS dup_group (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )";
    static constexpr auto createDupGroupMapTable = R"(
        CREATE TABLE IF NOT EXISTS dup_group_map (
            group_id INTEGER NOT NULL,
            video_id INTEGER NOT NULL,
            PRIMARY KEY (group_id, video_id),
            FOREIGN KEY (group_id) REFERENCES dup_group(id) ON DELETE CASCADE,
            FOREIGN KEY (video_id) REFERENCES video(id) ON DELETE CASCADE
        );
    )";

    static constexpr auto createSettingsTableSQL = R"(
        CREATE TABLE IF NOT EXISTS app_settings (
            id       INTEGER PRIMARY KEY CHECK (id = 1),
            json_blob TEXT NOT NULL
        );
    )";

    execStatement(createVideoTableSQL);
    execStatement(createHashTableSQL);
    execStatement(createDupGroupTable);
    execStatement(createDupGroupMapTable);
    execStatement(createSettingsTableSQL);
}

void DatabaseManager::execStatement(std::string const& sql)
{
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string msg = errMsg ? errMsg : "Unknown error";
        sqlite3_free(errMsg);
        spdlog::error("SQLite exec failed: {}", msg);
        throw std::runtime_error("SQLite exec failed: " + msg);
    }
}

void DatabaseManager::rollback()
{
    execStatement("ROLLBACK;");
}

void DatabaseManager::commit()
{
    execStatement("COMMIT;");
}

void DatabaseManager::beginTransaction()
{
    execStatement("BEGIN TRANSACTION;");
}

SearchSettings DatabaseManager::loadSettings() const
{
    static constexpr auto sql = "SELECT json_blob FROM app_settings WHERE id=1;";
    auto stmt = prepareStatement(m_db, sql);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        auto* txt = reinterpret_cast<char const*>(sqlite3_column_text(stmt.get(), 0));
        if (txt) {
            return nlohmann::json::parse(txt).get<SearchSettings>();
        }
    }
    return {};
}

void DatabaseManager::saveSettings(SearchSettings const& s)
{
    nlohmann::json j = s;
    std::string blob = j.dump();
    static constexpr auto sql = "REPLACE INTO app_settings (id, json_blob) VALUES (1, ?);";
    auto stmt = prepareStatement(m_db, sql);
    checkRc(sqlite3_bind_text(stmt.get(), 1, blob.c_str(), -1, SQLITE_TRANSIENT),
        m_db, "bind settings json");
    checkRc(sqlite3_step(stmt.get()), m_db, "save settings");
}
