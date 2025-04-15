#pragma once

#include <cstring>
#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <iostream>
#include <vector>
#include "Hash.h"
#include "VideoInfo.h"

class DatabaseManager {
public:
    explicit DatabaseManager(std::string const& dbPath) {
        if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK) {
            throw std::runtime_error("Cannot open database: " + std::string(sqlite3_errmsg(m_db)));
        }
        execStatement("PRAGMA foreign_keys = ON;");
        initDatabase();
    }

    ~DatabaseManager() {
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
    }

    DatabaseManager(DatabaseManager const&) = delete;
    DatabaseManager& operator=(DatabaseManager const&) = delete;

    void insertVideo(VideoInfo &video) {
        static char const* sql = R"(
            INSERT INTO video (
                path, created_at, modified_at, 
                video_codec, audio_codec, width, height, 
                duration, size, bit_rate, num_hard_links, 
                inode, device, sample_rate_avg, avg_frame_rate, thumbnail_path
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);
        )";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[DB] Failed to prepare insertVideo: " << sqlite3_errmsg(m_db) << "\n";
            return;
        }

        sqlite3_bind_text  (stmt,   1, video.path.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt,   2, video.created_at.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt,   3, video.modified_at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt,   4, video.video_codec.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt,   5, video.audio_codec.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt,   6, video.width);
        sqlite3_bind_int   (stmt,   7, video.height);
        sqlite3_bind_int   (stmt,   8, video.duration);
        sqlite3_bind_int   (stmt,   9, video.size);
        sqlite3_bind_int   (stmt,  10, video.bit_rate);
        sqlite3_bind_int   (stmt,  11, video.num_hard_links);
        sqlite3_bind_int64 (stmt,  12, static_cast<sqlite3_int64>(video.inode));
        sqlite3_bind_int64 (stmt,  13, static_cast<sqlite3_int64>(video.device));
        sqlite3_bind_int   (stmt,  14, video.sample_rate_avg);
        sqlite3_bind_double(stmt,  15, video.avg_frame_rate);
        sqlite3_bind_text  (stmt,  16, video.thumbnail_path.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "[DB] Failed to insert video: " << sqlite3_errmsg(m_db) << "\n";
        }

        sqlite3_finalize(stmt);

        video.id = static_cast<int>(sqlite3_last_insert_rowid(m_db));
    }

    void insertAllHashes(int video_id, std::vector<uint64_t> const& pHashes) {
        if (pHashes.empty()) {
            return;
        }

        static char const* sql = R"(
            INSERT INTO hash (video_id, hash_blob)
            VALUES (?, ?);
        )";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[DB] Failed to prepare insertAllHashes: " << sqlite3_errmsg(m_db) << "\n";
            return;
        }

        sqlite3_bind_int(stmt, 1, video_id);

        size_t numBytes = pHashes.size() * sizeof(unsigned long long);
        // It's safe to reinterpret_cast directly from &pHashes[0]
        // as they are contiguous in memory
        void const* dataPtr = static_cast<void const*>(pHashes.data());

        // 3) Bind the blob
        // Flags: SQLITE_TRANSIENT means SQLite makes its own copy
        sqlite3_bind_blob(stmt, 2, dataPtr, static_cast<int>(numBytes), SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "[DB] Failed to insert hash blob: " << sqlite3_errmsg(m_db) << "\n";
        }

        sqlite3_finalize(stmt);
    }

    std::vector<VideoInfo> getAllVideos() const {
        std::vector<VideoInfo> results;

        constexpr const char* sql = R"(
            SELECT id, path, created_at, modified_at,
                   video_codec, audio_codec, width, height,
                   duration, size, bit_rate, num_hard_links,
                   inode, device, sample_rate_avg, avg_frame_rate, thumbnail_path
            FROM video
            ORDER BY id ASC;
        )";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[DB] Failed to prepare SELECT getAllVideos: " << sqlite3_errmsg(m_db) << '\n';
            return results;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            VideoInfo v;
            v.id               = sqlite3_column_int(stmt, 0);
            v.path             = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            v.created_at       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            v.modified_at      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            v.video_codec      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            v.audio_codec      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            v.width            = sqlite3_column_int(stmt, 6);
            v.height           = sqlite3_column_int(stmt, 7);
            v.duration         = sqlite3_column_int(stmt, 8);
            v.size             = sqlite3_column_int(stmt, 9);
            v.bit_rate         = sqlite3_column_int(stmt, 10);
            v.num_hard_links   = sqlite3_column_int(stmt, 11);
            v.inode            = static_cast<long>(sqlite3_column_int64(stmt, 12));
            v.device           = static_cast<long>(sqlite3_column_int64(stmt, 13));
            v.sample_rate_avg  = sqlite3_column_int(stmt, 14);
            v.avg_frame_rate   = sqlite3_column_double(stmt, 15);
            const char* thumbPtr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
            v.thumbnail_path = thumbPtr ? thumbPtr : "";

            results.push_back(std::move(v));
        }

        sqlite3_finalize(stmt);
        return results;
    }


    std::vector<HashGroup> getAllHashGroups() const {
        std::vector<HashGroup> results;

        constexpr const char* sql = R"(
            SELECT video_id, hash_blob 
            FROM hash
        )";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[DB] Failed to prepare SELECT getAllHashGroups: " << sqlite3_errmsg(m_db) << '\n';
            return results;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int videoId = sqlite3_column_int(stmt, 0);
            const void* blobPtr = sqlite3_column_blob(stmt, 1);
            int blobSize = sqlite3_column_bytes(stmt, 1);

            if (!blobPtr || blobSize <= 0) continue;

            size_t count = blobSize / sizeof(uint64_t);
            const auto* raw = static_cast<const uint64_t*>(blobPtr);

            HashGroup group;
            group.fk_hash_video = videoId;
            group.hashes.assign(raw, raw + count);

            results.push_back(std::move(group));
        }

        sqlite3_finalize(stmt);
        return results;
    }

private:
    sqlite3* m_db = nullptr;

    void initDatabase() {
        std::string const createVideoTableSQL = R"(
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

        std::string const createHashTableSQL = R"(
            CREATE TABLE IF NOT EXISTS hash (
                video_id INTEGER PRIMARY KEY,
                hash_blob BLOB NOT NULL,
                FOREIGN KEY(video_id) REFERENCES video(id) ON DELETE CASCADE
            );
        )";

        execStatement(createVideoTableSQL);
        execStatement(createHashTableSQL);
    }

    void execStatement(std::string const& sql) {
        char* errMsg = nullptr;
        if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::string msg = errMsg ? errMsg : "Unknown error";
            sqlite3_free(errMsg);
            throw std::runtime_error("[DB] SQLite error: " + msg);
        }
    }
};

