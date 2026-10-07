#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace git_ops {
    struct SourceSnapshot {
        std::filesystem::path repository;
        std::string baseline;
        std::string head;
        std::optional<std::string> branch;
    };

    auto inspect_source(const std::filesystem::path &requested, const std::string &revision)
        -> SourceSnapshot;
    void initialize_snapshot(const SourceSnapshot &source,
                             const std::filesystem::path &workspace_directory);
    auto status(const std::filesystem::path &workspace_directory) -> std::string;
    auto diff(const std::filesystem::path &workspace_directory, const std::string &baseline)
        -> std::string;

} // namespace git_ops
