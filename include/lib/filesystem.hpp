#pragma once

#include <filesystem>

namespace lib {
    void require_directory(const std::filesystem::path &path);
    void validate_relative_path(const std::filesystem::path &path);
    auto resolve_relative_path(const std::filesystem::path &root,
                               const std::filesystem::path &relative) -> std::filesystem::path;
    // Open a path below an existing directory FD; reject symlinks and mount crossings.
    auto open_beneath(int directory, const std::filesystem::path &path, int flags) -> int;
} // namespace lib
