#include "ConfigManager.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using namespace std::string_literals;
static char const* kCfgFile = "config.json";

namespace cfg {
std::string defaultDatabasePath() { return "videos.db"; }

std::optional<std::string> loadDatabasePath()
{
    std::ifstream in(kCfgFile, std::ios::binary);
    if (!in)
        return std::nullopt;

    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("dbPath") && j["dbPath"].is_string()) {
            std::string p = j["dbPath"].get<std::string>();
            if (!p.empty())
                return p;
        }
    } catch (std::exception const& e) {
        spdlog::warn("Failed to read {}: {}", kCfgFile, e.what());
    }
    return std::nullopt;
}

void saveDatabasePath(std::string const& p)
{
    try {
        nlohmann::json j = { { "dbPath", p } };
        std::ofstream out(kCfgFile, std::ios::binary | std::ios::trunc);
        out << j.dump(4);
    } catch (std::exception const& e) {
        spdlog::error("Failed to write {}: {}", kCfgFile, e.what());
    }
}
} // namespace cfg
