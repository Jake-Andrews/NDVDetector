#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

std::vector<std::filesystem::path>
get_files_with_extensions(std::filesystem::path const& root,
    std::unordered_set<std::string> const& extensions)
{
    std::vector<std::filesystem::path> result;

    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return result;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             ec);
        it != std::filesystem::recursive_directory_iterator();
        it.increment(ec)) {

        if (ec)
            continue;

        if (std::filesystem::is_regular_file(*it, ec)) {
            auto ext = it->path().extension().string();
            if (!ext.empty() && extensions.contains(ext)) {
                result.push_back(std::filesystem::absolute(it->path(), ec).lexically_normal());
            }
        }
    }

    return result;
}
