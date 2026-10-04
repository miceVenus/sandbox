#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace git_storage {
    struct SourceSnapshot {
        std::filesystem::path repository;
        std::string baseline;
        std::string head;
        std::optional<std::string> branch;
    };

    SourceSnapshot inspect_source(const std::filesystem::path &requested,
                                  const std::string &revision);
    void initialize_snapshot(const SourceSnapshot &source, const std::filesystem::path &session);
    std::string status(const std::filesystem::path &session);
    std::string diff(const std::filesystem::path &session, const std::string &baseline);

    // Worker implementation. Only the SDK-owned sandbox-git executable links it.
    namespace worker {
        SourceSnapshot inspect_source(const std::filesystem::path &requested,
                                      const std::string &revision);
        void initialize_snapshot(const SourceSnapshot &source,
                                 const std::filesystem::path &session);
        std::string status(const std::filesystem::path &session);
        std::string diff(const std::filesystem::path &session, const std::string &baseline);
    } // namespace worker
} // namespace git_storage
