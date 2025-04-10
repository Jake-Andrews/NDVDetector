#pragma once

#include <filesystem>
#include <unordered_set>
#include <vector>

std::vector<std::filesystem::path> get_files_with_extensions(std::filesystem::path const& root, std::unordered_set<std::string> const& extensions);
